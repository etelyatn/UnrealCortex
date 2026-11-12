#include "CortexReplayService.h"

#include "CortexReplayServiceImpl.h"

#include "CortexReplayErrorCodes.h"
#include "Widgets/SCortexReplayRunOverlay.h"

#include "Dom/JsonValue.h"

using namespace CortexReplayServicePrivate;

namespace
{
constexpr int32 ServiceRecentTerminalRunLimit = 100;
constexpr double ServiceRecentTerminalWindowSeconds = 86400.0;
}

void FCortexReplayService::FImpl::EnsureTicker()
{
	if (TickerHandle.IsValid() || bShutdown)
	{
		return;
	}
	// The ticker holds a shared reference to this state so an in-flight finalization can never
	// outlive the memory that owns it.
	const TSharedRef<FCortexReplayService::FImpl> Self = AsShared();
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Self](float DeltaSeconds)
		{
			return Self->Tick(DeltaSeconds);
		}));
}

void FCortexReplayService::FImpl::DetachTickerAndSession()
{
	RemoveRunOverlay();
	// The frame observer detaches from its exact session in its own destructor, so it must be
	// uninstalled while that session is still alive. Uninstall clears the session link, which makes a
	// later destructor call harmless.
	ReleaseCaptureFrameObserver();
	// Teardown must not leave the engine on a recording's fixed step either.
	RemoveFrameHooks();
	Clock.Release();
	FCortexEditorPhysicalInputSession::ShutdownAndRetainUntilOwnedTeardown(MoveTemp(Session));
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}

void FCortexReplayService::FImpl::SetRunState(ECortexReplayState NewState)
{
	RunState = NewState;
	ActiveRun.State = NewState;
}

bool FCortexReplayService::FImpl::Tick(float DeltaSeconds)
{
	(void)DeltaSeconds;
	if (bShutdown)
	{
		// The owning service may already be destroyed: only the lifetime-owned finalization
		// bookkeeping runs, and the ticker stops as soon as nothing is pending.
		TickCapture();
		TickFinalization();
		return bRunActive || bFinalizing || CapturePhase != ECapturePhase::None;
	}
	TickRun();
	TickCapture();
	TickFinalization();
	return true;
}

FCortexReplayService::FCortexReplayService(const FString& ProjectRoot)
	: Impl(MakeShared<FImpl>(this, ProjectRoot))
{
	Impl->EnsureTicker();
}

FCortexReplayService::~FCortexReplayService()
{
	Shutdown();
}

FCortexCommandResult FCortexReplayService::GetRun(const FGuid& Id, bool bAIOnly) const
{
	const FImpl& State = *Impl;
	const bool bMatchesActive = State.ActiveRun.Id == Id
		&& (!bAIOnly || State.ActiveRun.Origin == ECortexReplayOrigin::AI);
	if (bMatchesActive && (State.bRunActive || State.bFinalizing || State.bTerminalPublished))
	{
		return FCortexCommandRouter::Success(State.BuildActiveRunData());
	}

	// A pre-frame record has no frame identity to report, so its full detail is unsupported while it
	// stays discoverable through the compact recovery summaries.
	auto RetainedResult = [](const FCortexReplayRunRecord& Retained) -> FCortexCommandResult
	{
		if (Retained.bLegacyFormat)
		{
			return ServiceError(CortexReplayErrorCodes::UnsupportedRunFormat,
				TEXT("Historical run records carry no frame identity"));
		}
		return FCortexCommandRouter::Success(BuildRunData(Retained, false, nullptr));
	};

	FCortexReplayRunRecord Record;
	if (State.RunStore.Load(Id, ECortexReplayOrigin::AI, Record).bSuccess)
	{
		return RetainedResult(Record);
	}
	// An unrestricted human query resolves retained runs of either origin.
	if (!bAIOnly && State.RunStore.Load(Id, ECortexReplayOrigin::Human, Record).bSuccess)
	{
		return RetainedResult(Record);
	}
	return ServiceError(CortexReplayErrorCodes::RunNotFound,
		FString::Printf(TEXT("No replay run %s"), *ServiceGuidToString(Id)));
}

FCortexCommandResult FCortexReplayService::GetLastRunForRecording(int32 Id) const
{
	bool bFound = false;
	FCortexReplayRunRecord Record;
	const FCortexCommandResult Loaded = Impl->RunStore.GetLatestForRecording(Id, bFound, Record);
	if (!Loaded.bSuccess)
	{
		return Loaded;
	}
	if (!bFound)
	{
		return ServiceSuccess();
	}
	return FCortexCommandRouter::Success(BuildRunData(Record, false, nullptr));
}

FCortexCommandResult FCortexReplayService::GetCurrentOperation() const
{
	const FImpl& State = *Impl;
	if (!State.bRunActive && State.CapturePhase == FImpl::ECapturePhase::None)
	{
		return ServiceSuccess();
	}

	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	if (State.bRunActive)
	{
		Data->SetStringField(TEXT("kind"), TEXT("replay"));
		Data->SetStringField(TEXT("origin"), ServiceOriginToString(State.ActiveRun.Origin));
		Data->SetNumberField(TEXT("recording_id"), State.ActiveRun.RecordingId);
		Data->SetStringField(TEXT("run_id"), ServiceGuidToString(State.ActiveRun.Id));
		Data->SetStringField(TEXT("state"), ServiceStateToString(State.RunState));
		Data->SetObjectField(TEXT("guard_coverage"), ServiceGuardCoverageToJson(State.ActiveRun.GuardCoverage));
		Data->SetNumberField(TEXT("dispatched_events"), State.ActiveRun.DispatchedEvents);
		Data->SetNumberField(TEXT("total_events"), State.ActiveRun.TotalEvents);
		Data->SetNumberField(TEXT("authorized_wait_seconds"), State.ActiveRun.AuthorizedWaitSeconds);
		Data->SetNumberField(TEXT("behind_seconds"), State.ActiveRun.BehindSeconds);
		State.AddPhaseFields(*Data);
		if (State.IsLiveReplaying() && State.Scheduler.IsValid()
			&& State.Scheduler->GetWaitingSequence() != INDEX_NONE)
		{
			Data->SetObjectField(TEXT("waiting"), ServiceWaitingToJson(*State.Scheduler));
		}
		else
		{
			Data->SetField(TEXT("waiting"), MakeShared<FJsonValueNull>());
		}
	}
	else
	{
		const TCHAR* CaptureState =
			State.CapturePhase == FImpl::ECapturePhase::Preparing ? TEXT("Preparing")
			: State.CapturePhase == FImpl::ECapturePhase::Finalizing ? TEXT("Finalizing")
			: TEXT("Recording");
		Data->SetStringField(TEXT("kind"), TEXT("capture"));
		Data->SetStringField(TEXT("origin"), TEXT("human"));
		Data->SetNumberField(TEXT("recording_id"), State.CaptureRecordingId);
		Data->SetStringField(TEXT("state"), CaptureState);
		// A pending or failed publication is visible to the human window while retries are pending.
		Data->SetBoolField(TEXT("publication_pending"),
			State.bCapturePublishOnComplete && !State.bCaptureFaulted);
		Data->SetBoolField(TEXT("publication_failed"), State.bCapturePublicationFailed);
		// A capture fault (with the reason that caused it) stays visible to the human window while
		// the capture finalizes without publishing.
		Data->SetBoolField(TEXT("faulted"), State.bCaptureFaulted);
		if (State.bCaptureFaulted)
		{
			TSharedRef<FJsonObject> FaultError = MakeShared<FJsonObject>();
			FaultError->SetStringField(TEXT("code"), State.CaptureFaultResult.ErrorCode);
			FaultError->SetStringField(TEXT("message"), State.CaptureFaultResult.ErrorMessage);
			Data->SetObjectField(TEXT("fault_error"), FaultError);
		}
		else
		{
			Data->SetField(TEXT("fault_error"), MakeShared<FJsonValueNull>());
		}
		if (State.bCapturePublicationFailed)
		{
			TSharedRef<FJsonObject> PublicationError = MakeShared<FJsonObject>();
			PublicationError->SetStringField(TEXT("code"), State.CapturePublicationError.ErrorCode);
			PublicationError->SetStringField(TEXT("message"), State.CapturePublicationError.ErrorMessage);
			Data->SetObjectField(TEXT("publication_error"), PublicationError);
		}
		else
		{
			Data->SetField(TEXT("publication_error"), MakeShared<FJsonValueNull>());
		}
	}
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexReplayService::GetLastCaptureResult() const
{
	const FImpl& State = *Impl;
	// Human-UI status only: a null success when no capture has completed, otherwise the retained
	// terminal summary. The active operation is never reported here.
	if (!State.LastCaptureOutcome.IsValid())
	{
		return FCortexCommandRouter::Success(nullptr);
	}
	return FCortexCommandRouter::Success(State.LastCaptureOutcome);
}

bool FCortexReplayService::IsRecordInUse(int32 Id) const
{
	const FImpl& State = *Impl;
	if (State.CapturePhase != FImpl::ECapturePhase::None && State.CaptureRecordingId == Id)
	{
		return true;
	}
	if ((State.bRunActive || State.bFinalizing) && State.ActiveRun.RecordingId == Id)
	{
		return true;
	}
	return false;
}

FString FCortexReplayService::GetRecordingDisplayName(int32 Id) const
{
	if (Impl->Snapshot.IsValid() && Impl->Snapshot->Metadata.RecordingId == Id)
	{
		return Impl->Snapshot->Metadata.Name;
	}
	FCortexReplayMetadata Metadata;
	return Impl->Library.ReadMetadata(Id, Metadata) ? Metadata.Name : FString();
}

FCortexCommandResult FCortexReplayService::GetRecording(int32 Id, bool bAIOnly) const
{
	TSharedPtr<const FCortexReplaySnapshot> LoadedSnapshot;
	const FCortexCommandResult Loaded = Impl->Library.Load(Id, bAIOnly, LoadedSnapshot);
	if (!Loaded.bSuccess)
	{
		return Loaded;
	}

	TSharedRef<FJsonObject> Data = ServiceMetadataToJson(LoadedSnapshot->Metadata);
	TSharedRef<FJsonObject> InitialState = MakeShared<FJsonObject>();
	InitialState->SetNumberField(TEXT("schema_version"), LoadedSnapshot->InitialState.SchemaVersion);
	InitialState->SetNumberField(TEXT("recording_id"), LoadedSnapshot->InitialState.RecordingId);
	InitialState->SetStringField(TEXT("pawn_class_path"), LoadedSnapshot->InitialState.PawnClassPath);

	const FCortexEditorPhysicalInputPlayerPose& Pose = LoadedSnapshot->InitialState.Pose;
	const FVector Location = Pose.PawnTransform.GetLocation();
	TSharedRef<FJsonObject> PawnTransform = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> LocationJson = MakeShared<FJsonObject>();
	LocationJson->SetNumberField(TEXT("x"), Location.X);
	LocationJson->SetNumberField(TEXT("y"), Location.Y);
	LocationJson->SetNumberField(TEXT("z"), Location.Z);
	PawnTransform->SetObjectField(TEXT("location_cm"), LocationJson);
	const FRotator PawnRotation = Pose.PawnTransform.Rotator();
	TSharedRef<FJsonObject> PawnRotationJson = MakeShared<FJsonObject>();
	PawnRotationJson->SetNumberField(TEXT("pitch"), PawnRotation.Pitch);
	PawnRotationJson->SetNumberField(TEXT("yaw"), PawnRotation.Yaw);
	PawnRotationJson->SetNumberField(TEXT("roll"), PawnRotation.Roll);
	PawnTransform->SetObjectField(TEXT("rotation_deg"), PawnRotationJson);
	const FVector Scale = Pose.PawnTransform.GetScale3D();
	TSharedRef<FJsonObject> ScaleJson = MakeShared<FJsonObject>();
	ScaleJson->SetNumberField(TEXT("x"), Scale.X);
	ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
	ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
	PawnTransform->SetObjectField(TEXT("scale"), ScaleJson);
	InitialState->SetObjectField(TEXT("pawn_transform"), PawnTransform);

	TSharedRef<FJsonObject> ControlRotation = MakeShared<FJsonObject>();
	ControlRotation->SetNumberField(TEXT("pitch"), Pose.ControlRotation.Pitch);
	ControlRotation->SetNumberField(TEXT("yaw"), Pose.ControlRotation.Yaw);
	ControlRotation->SetNumberField(TEXT("roll"), Pose.ControlRotation.Roll);
	InitialState->SetObjectField(TEXT("control_rotation_deg"), ControlRotation);

	Data->SetObjectField(TEXT("initial_state"), InitialState);
	Data->SetStringField(TEXT("recording_snapshot_sha256"), LoadedSnapshot->RecordingSnapshotSha256);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexReplayService::ListRecordings(int32 AfterId, int32 PageSize) const
{
	TArray<FCortexReplayMetadata> Metadata;
	bool bHasMore = false;
	// Discovery is AI-only eligibility; human listing keeps its unrestricted API.
	const FCortexCommandResult Listed = Impl->Library.ListPage(true, AfterId, PageSize, Metadata, bHasMore);
	if (!Listed.bSuccess)
	{
		return Listed;
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Metadata.Num());
	for (const FCortexReplayMetadata& Row : Metadata)
	{
		Rows.Add(MakeShared<FJsonValueObject>(ServiceMetadataToJson(Row)));
	}

	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("editor_instance_id"), Impl->EditorInstanceId);
	Data->SetArrayField(TEXT("recordings"), Rows);
	Data->SetBoolField(TEXT("has_more"), bHasMore);
	if (bHasMore && Metadata.Num() > 0)
	{
		Data->SetNumberField(TEXT("next_after_recording_id"), Metadata.Last().RecordingId);
	}
	else
	{
		Data->SetField(TEXT("next_after_recording_id"), MakeShared<FJsonValueNull>());
	}

	TArray<FCortexReplayRunRecord> Recent;
	Impl->RunStore.ListRecent(ECortexReplayOrigin::AI, ServiceRecentTerminalRunLimit,
		ServiceRecentTerminalWindowSeconds, Recent);
	TArray<TSharedPtr<FJsonValue>> RecentRows;
	RecentRows.Reserve(Recent.Num());
	for (const FCortexReplayRunRecord& Run : Recent)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("run_id"), ServiceGuidToString(Run.Id));
		Summary->SetNumberField(TEXT("recording_id"), Run.RecordingId);
		Summary->SetStringField(TEXT("editor_instance_id"), Run.EditorInstanceId);
		Summary->SetStringField(TEXT("started_at_utc"), ServiceIsoUtc(Run.StartedAtUtc));
		Summary->SetStringField(TEXT("finalized_at_utc"), ServiceIsoUtc(Run.FinalizedAtUtc));
		Summary->SetStringField(TEXT("state"), ServiceStateToString(Run.State));
		RecentRows.Add(MakeShared<FJsonValueObject>(Summary));
	}
	Data->SetArrayField(TEXT("recent_ai_runs"), RecentRows);
	Data->SetNumberField(TEXT("recovery_window_seconds"), ServiceRecentTerminalWindowSeconds);
	Data->SetNumberField(TEXT("recovery_max_terminal_runs"), ServiceRecentTerminalRunLimit);
	if (Impl->bRunActive && Impl->ActiveRun.Origin == ECortexReplayOrigin::AI)
	{
		Data->SetField(TEXT("active_ai_run"), MakeShared<FJsonValueObject>(
			Impl->BuildActiveRunData()));
	}
	else
	{
		Data->SetField(TEXT("active_ai_run"), MakeShared<FJsonValueNull>());
	}
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexReplayService::ListHumanRecordingEntries(
	TArray<FCortexReplayLibraryEntry>& Out) const
{
	return Impl->Library.ListEntries(Out);
}

int64 FCortexReplayService::GetLibraryRevision() const
{
	return Impl->Library.GetRevision();
}

FCortexCommandResult FCortexReplayService::SaveMetadata(int32 Id, const FString& Name,
	const FString& Description, bool bAIEnabled, bool bCountdownEnabled)
{
	FImpl& State = *Impl;
	const FCortexCommandResult Saved =
		State.Library.SaveMetadata(Id, Name, Description, bAIEnabled, bCountdownEnabled);
	if (!Saved.bSuccess)
	{
		return Saved;
	}

	// Revocation cancels only a still-active AI run of that recording.
	if (!bAIEnabled && State.ActiveRun.Origin == ECortexReplayOrigin::AI
		&& State.ActiveRun.RecordingId == Id)
	{
		State.bPermissionRevoked = true;
		State.PermissionRevocationResult = ServiceError(CortexReplayErrorCodes::PermissionDenied,
			TEXT("Replay permission was revoked"));
		if (State.bRunActive && !State.bFinalizing)
		{
			State.bCancellationRequested = true;
			State.CancellationResult = State.PermissionRevocationResult;
			Finalize(ECortexReplayState::Cancelled, State.PermissionRevocationResult);
		}
	}
	return Saved;
}

FCortexCommandResult FCortexReplayService::DeleteRecording(int32 Id)
{
	// Refusal is side-effect-free; an actual external deletion during playback is picked up by the
	// live eligibility check, which cancels only an AI run whose recording disappeared.
	if (IsRecordInUse(Id))
	{
		return ServiceError(CortexErrorCodes::EditorBusy, TEXT("Recording is in use"));
	}
	return Impl->Library.Delete(Id);
}

void FCortexReplayService::SetCountdownSecondsForTesting(double Seconds)
{
	if (Impl.IsValid())
	{
		Impl->CountdownSeconds = FMath::Max(0.0, Seconds);
	}
}

TSharedPtr<SWidget> FCortexReplayService::GetRunOverlayForTesting() const
{
	return Impl.IsValid() ? TSharedPtr<SWidget>(Impl->RunOverlay) : nullptr;
}

FOnCortexReplayRunTerminal& FCortexReplayService::OnRunTerminal()
{
	return Impl->RunTerminalDelegate;
}

FOnCortexReplayRunAdmitted& FCortexReplayService::OnRunAdmitted()
{
	return Impl->RunAdmittedDelegate;
}

void FCortexReplayService::Shutdown()
{
	if (!Impl.IsValid())
	{
		return;
	}
	FImpl& State = *Impl;
	if (State.bShutdown)
	{
		return;
	}
	State.bShutdown = true;

	// An active capture terminates abnormally and never publishes a partial record.
	if (State.CapturePhase != FImpl::ECapturePhase::None
		&& State.CapturePhase != FImpl::ECapturePhase::Finalizing)
	{
		State.MarkCaptureFaulted(ServiceError(CortexReplayErrorCodes::InvalidOperation,
			TEXT("Replay service shut down")));
		State.BeginCaptureFinalization(false);
		State.TickCapture();
	}

	// An active run is routed through the single finalizer so exactly one terminal record is
	// retained, instead of dropping ownership without a persisted result.
	if (State.bRunActive && !State.bFinalizing)
	{
		State.bCancellationRequested = true;
		State.CancellationResult = ServiceError(CortexReplayErrorCodes::InvalidOperation,
			TEXT("Replay service shut down"));
		Finalize(ECortexReplayState::Cancelled, State.CancellationResult);
	}
	else if (State.bFinalizing)
	{
		State.TickFinalization();
	}

	// The finalization ticker (and the session it observes) is retained until the owned teardown
	// completes; only then is everything released.
	if (!State.bRunActive && !State.bFinalizing && State.CapturePhase == FImpl::ECapturePhase::None)
	{
		State.DetachTickerAndSession();
	}
}

void FCortexReplayService::ShutdownForModuleUnload()
{
	Shutdown();
	if (!Impl.IsValid())
	{
		return;
	}
	FImpl& State = *Impl;
	State.bUnloading = true;
	if (State.bFinalizing)
	{
		State.TickFinalization();
	}
	if (State.CapturePhase == FImpl::ECapturePhase::Finalizing)
	{
		State.TickCapture();
	}
	State.DetachTickerAndSession();
}

bool FCortexReplayService::IsQuiescent() const
{
	if (!Impl.IsValid())
	{
		return true;
	}
	const FImpl& State = *Impl;
	return !State.TickerHandle.IsValid()
		&& !State.BeginFrameHandle.IsValid()
		&& !State.SamplingInputHandle.IsValid()
		&& !State.WorldTickStartHandle.IsValid()
		&& !State.Session.IsValid()
		&& !State.CaptureFrameObserver.IsValid();
}
