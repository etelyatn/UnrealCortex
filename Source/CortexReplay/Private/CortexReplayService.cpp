#include "CortexReplayService.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInputSession.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayGuardEvaluator.h"
#include "CortexReplayLibrary.h"
#include "CortexReplayModule.h"
#include "CortexReplayRunStore.h"
#include "CortexReplayScheduler.h"
#include "CortexReplayTypes.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"
#include "Templates/Function.h"
#include "UObject/Package.h"

namespace
{
/** The service's monotonic preparation deadline starts at native acceptance. */
constexpr double ServicePreparationDeadlineSeconds = 30.0;

constexpr int32 ServiceRecentTerminalRunLimit = 100;
constexpr double ServiceRecentTerminalWindowSeconds = 86400.0;

FString ServiceGuidToString(const FGuid& Id)
{
	return Id.ToString(EGuidFormats::DigitsWithHyphens).ToLower();
}

const TCHAR* ServiceStateToString(ECortexReplayState State)
{
	switch (State)
	{
	case ECortexReplayState::Preparing: return TEXT("Preparing");
	case ECortexReplayState::Recording: return TEXT("Recording");
	case ECortexReplayState::Replaying: return TEXT("Replaying");
	case ECortexReplayState::Finalizing: return TEXT("Finalizing");
	case ECortexReplayState::Completed: return TEXT("Completed");
	case ECortexReplayState::Cancelled: return TEXT("Cancelled");
	case ECortexReplayState::Interrupted: return TEXT("Interrupted");
	case ECortexReplayState::Error: return TEXT("Error");
	default: return TEXT("Error");
	}
}

const TCHAR* ServiceOriginToString(ECortexReplayOrigin Origin)
{
	return Origin == ECortexReplayOrigin::AI ? TEXT("ai") : TEXT("human");
}

const TCHAR* ServiceObservationStateToString(ECortexEditorUIObservationState State)
{
	switch (State)
	{
	case ECortexEditorUIObservationState::Ready: return TEXT("ready");
	case ECortexEditorUIObservationState::PointerPending: return TEXT("pointer_pending");
	case ECortexEditorUIObservationState::LayoutPending: return TEXT("layout_pending");
	case ECortexEditorUIObservationState::TargetMissing: return TEXT("target_missing");
	case ECortexEditorUIObservationState::TargetDisabled: return TEXT("target_disabled");
	case ECortexEditorUIObservationState::TargetNotHitTestable: return TEXT("target_not_hit_testable");
	case ECortexEditorUIObservationState::WrongTarget: return TEXT("wrong_target");
	case ECortexEditorUIObservationState::Ambiguous: return TEXT("ambiguous");
	case ECortexEditorUIObservationState::Unavailable: return TEXT("unavailable");
	default: return TEXT("unknown");
	}
}

FCortexCommandResult ServiceSuccess()
{
	return FCortexCommandRouter::Success(nullptr);
}

FCortexCommandResult ServiceError(const TCHAR* Code, const FString& Message)
{
	return FCortexCommandRouter::Error(FString(Code), Message);
}

FCortexCommandResult ServiceError(const FString& Code, const FString& Message)
{
	return FCortexCommandRouter::Error(Code, Message);
}

FString ServiceIsoUtc(const FDateTime& Time)
{
	return Time.GetTicks() > 0 ? Time.ToIso8601() : FString();
}

TSharedRef<FJsonObject> ServiceGuardCoverageToJson(const FCortexReplayGuardCoverage& Coverage)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("scope"), TEXT("press_only"));
	Object->SetNumberField(TEXT("pose_presses"), Coverage.PosePresses);
	Object->SetNumberField(TEXT("ui_supported_presses"), Coverage.UISupportedPresses);
	Object->SetNumberField(TEXT("ui_unavailable_presses"), Coverage.UIUnavailablePresses);
	Object->SetNumberField(TEXT("ui_not_applicable_presses"), Coverage.UINotApplicablePresses);
	return Object;
}

TSharedRef<FJsonObject> ServiceMetadataToJson(const FCortexReplayMetadata& Metadata)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("format"), TEXT("CortexReplay"));
	Object->SetNumberField(TEXT("schema_version"), Metadata.SchemaVersion);
	Object->SetNumberField(TEXT("recording_id"), Metadata.RecordingId);
	Object->SetStringField(TEXT("name"), Metadata.Name);
	Object->SetStringField(TEXT("description"), Metadata.Description);
	Object->SetStringField(TEXT("map_asset_path"), Metadata.MapAssetPath);
	Object->SetStringField(TEXT("engine_version"), Metadata.EngineVersion);
	Object->SetStringField(TEXT("plugin_version"), Metadata.PluginVersion);
	Object->SetStringField(TEXT("created_at_utc"), ServiceIsoUtc(Metadata.CreatedAtUtc));
	Object->SetNumberField(TEXT("duration_seconds"), Metadata.DurationSeconds);
	Object->SetBoolField(TEXT("ai_enabled"), Metadata.bAIEnabled);
	Object->SetBoolField(TEXT("complete"), Metadata.bComplete);
	Object->SetStringField(TEXT("initial_state_sha256"), Metadata.InitialStateSha256);
	Object->SetStringField(TEXT("inputs_sha256"), Metadata.InputsSha256);

	TSharedRef<FJsonObject> Prerequisites = MakeShared<FJsonObject>();
	Prerequisites->SetNumberField(TEXT("local_player_index"), Metadata.Prerequisites.LocalPlayerIndex);
	TSharedRef<FJsonObject> ViewportSize = MakeShared<FJsonObject>();
	ViewportSize->SetNumberField(TEXT("x"), Metadata.Prerequisites.ViewportSize.X);
	ViewportSize->SetNumberField(TEXT("y"), Metadata.Prerequisites.ViewportSize.Y);
	Prerequisites->SetObjectField(TEXT("viewport_size"), ViewportSize);
	Prerequisites->SetNumberField(TEXT("dpi_scale"), Metadata.Prerequisites.DpiScale);
	Object->SetObjectField(TEXT("prerequisites"), Prerequisites);

	Object->SetObjectField(TEXT("guard_coverage"), ServiceGuardCoverageToJson(Metadata.GuardCoverage));
	return Object;
}
}

/** All mutable service state lives here so the public header stays declaration-only. */
struct FCortexReplayService::FImpl
{
	explicit FImpl(FCortexReplayService* InOwner, const FString& InProjectRoot)
		: Owner(InOwner)
		, ProjectRoot(InProjectRoot)
		, Library(InProjectRoot)
		, RunStore(InProjectRoot)
		, EditorInstanceId(ServiceGuidToString(FGuid::NewGuid()))
	{
		RunStore.Initialize();
	}

	FCortexReplayService* Owner = nullptr;
	FString ProjectRoot;
	FCortexReplayLibrary Library;
	FCortexReplayRunStore RunStore;
	FString EditorInstanceId;

	TSharedPtr<FCortexEditorPhysicalInputSession> Session;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bShutdown = false;

	// ---- capture ----
	bool bCaptureActive = false;
	bool bBorrowedCapture = false;
	int32 CaptureRecordingId = 0;
	FString CaptureMapAssetPath;
	FCortexEditorPhysicalInputTargetInfo CaptureTargetInfo;
	FCortexEditorPhysicalInputPlayerPose CaptureInitialPose;
	TArray<FCortexReplayEvent> CaptureEvents;
	double CaptureEpochSeconds = 0.0;

	// ---- run ----
	bool bRunActive = false;
	bool bFinalizing = false;
	bool bTerminalPublished = false;
	bool bFrozen = false;
	bool bOwnedPie = false;
	ECortexReplayState RunState = ECortexReplayState::Preparing;
	FCortexReplayRunRecord ActiveRun;
	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	TSharedPtr<FCortexReplayScheduler> Scheduler;
	double PrepareDeadline = 0.0;
	double ReplayEpoch = 0.0;
	bool bReadySeen = false;
	FCortexCommandResult PreparationResult;
	bool bCancellationRequested = false;
	bool bInterruptionRequested = false;
	FCortexCommandResult InterruptionResult;
	ECortexReplayState PendingTerminalState = ECortexReplayState::Cancelled;
	FCortexCommandResult PendingTerminalResult;

	bool Tick(float DeltaSeconds);
	void TickRun();
	void TickFinalization();
	void EnsureTicker();
	FCortexReplayGuardDecision EvaluateGuard(const FCortexReplayEvent& Event);
	FCortexCommandResult DispatchEvent(const FCortexReplayEvent& Event);
	void OnCaptureEvent(const FCortexEditorPhysicalInputEvent& Event, double TimeSeconds,
		const FCortexEditorPhysicalInputCaptureContext& Context);
	void SetRunState(ECortexReplayState NewState);
};

void FCortexReplayService::FImpl::EnsureTicker()
{
	if (TickerHandle.IsValid() || bShutdown)
	{
		return;
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FCortexReplayService::FImpl::Tick));
}

void FCortexReplayService::FImpl::SetRunState(ECortexReplayState NewState)
{
	RunState = NewState;
	ActiveRun.State = NewState;
}

bool FCortexReplayService::FImpl::Tick(float DeltaSeconds)
{
	(void)DeltaSeconds;
	TickRun();
	TickFinalization();
	return true;
}

void FCortexReplayService::FImpl::TickRun()
{
	if (!bRunActive || bFinalizing || !Snapshot.IsValid())
	{
		return;
	}

	if (RunState == ECortexReplayState::Preparing)
	{
		if (!Session.IsValid())
		{
			Owner->Finalize(ECortexReplayState::Error,
				ServiceError(CortexReplayErrorCodes::TargetUnavailable,
					TEXT("Replay target session is unavailable")));
			return;
		}

		// The service's deadline cannot be extended by the session's own deadline.
		if (FPlatformTime::Seconds() >= PrepareDeadline)
		{
			FCortexCommandResult Timeout = ServiceError(
				CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
				TEXT("Replay preparation exceeded the monotonic deadline"));
			Owner->Finalize(ECortexReplayState::Error, Timeout);
			return;
		}
		if (!bReadySeen)
		{
			return;
		}
		if (!PreparationResult.bSuccess)
		{
			UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s preparation failed: %s (%s)"),
				*ServiceGuidToString(ActiveRun.Id), *PreparationResult.ErrorCode,
				*PreparationResult.ErrorMessage);
			Owner->Finalize(ECortexReplayState::Error, PreparationResult);
			return;
		}

		// Recheck permission after readiness and before the epoch starts.
		if (ActiveRun.Origin == ECortexReplayOrigin::AI)
		{
			TSharedPtr<const FCortexReplaySnapshot> Recheck;
			const FCortexCommandResult Eligible = Library.Load(ActiveRun.RecordingId, true, Recheck);
			if (!Eligible.bSuccess)
			{
				UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s lost eligibility after readiness: %s (%s)"),
					*ServiceGuidToString(ActiveRun.Id), *Eligible.ErrorCode, *Eligible.ErrorMessage);
				Owner->Finalize(ECortexReplayState::Error, Eligible);
				return;
			}
		}

		// One-time pose restoration and immediate readback.
		const FCortexCommandResult Restored = Session->RestorePlayerPose(
			Snapshot->InitialState.Pose, Snapshot->InitialState.PawnClassPath);
		if (FPlatformTime::Seconds() >= PrepareDeadline)
		{
			FCortexCommandResult Timeout = ServiceError(
				CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
				TEXT("Replay preparation exceeded the monotonic deadline"));
			Owner->Finalize(ECortexReplayState::Error, Timeout);
			return;
		}
		if (!Restored.bSuccess)
		{
			UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s pose restoration failed: %s (%s)"),
				*ServiceGuidToString(ActiveRun.Id), *Restored.ErrorCode, *Restored.ErrorMessage);
			Owner->Finalize(ECortexReplayState::Error, Restored);
			return;
		}

		FCortexCommandResult TargetError;
		if (!Session->ValidateTarget(TargetError))
		{
			Owner->Finalize(ECortexReplayState::Interrupted, TargetError);
			return;
		}

		Scheduler = MakeShared<FCortexReplayScheduler>(Snapshot.ToSharedRef());
		ReplayEpoch = FPlatformTime::Seconds();
		SetRunState(ECortexReplayState::Replaying);
	}

	if (RunState == ECortexReplayState::Replaying)
	{
		if (bCancellationRequested)
		{
			Owner->Finalize(ECortexReplayState::Cancelled, ServiceSuccess());
			return;
		}
		if (bInterruptionRequested)
		{
			Owner->Finalize(ECortexReplayState::Interrupted, InterruptionResult);
			return;
		}

		const FCortexCommandResult Advanced = Scheduler->Advance(
			[this]() { return FPlatformTime::Seconds() - ReplayEpoch; },
			[this](const FCortexReplayEvent& Event) { return EvaluateGuard(Event); },
			[this](const FCortexReplayEvent& Event) { return DispatchEvent(Event); });

		ActiveRun.DispatchedEvents = Scheduler->GetDispatchedCount();
		ActiveRun.AuthorizedWaitSeconds = Scheduler->GetAuthorizedWaitSeconds();

		if (!Advanced.bSuccess)
		{
			UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s dispatch failed: %s (%s)"),
				*ServiceGuidToString(ActiveRun.Id), *Advanced.ErrorCode, *Advanced.ErrorMessage);
			if (bInterruptionRequested)
			{
				Owner->Finalize(ECortexReplayState::Interrupted, InterruptionResult);
			}
			else
			{
				Owner->Finalize(ECortexReplayState::Error, Advanced);
			}
			return;
		}

		if (Scheduler->IsComplete())
		{
			Owner->Finalize(ECortexReplayState::Completed, ServiceSuccess());
		}
	}
}

void FCortexReplayService::FImpl::TickFinalization()
{
	if (!bFinalizing)
	{
		return;
	}

	// Owned teardown is complete only when the matching request is cancelled and the owned
	// context has disappeared. Borrowed sessions report complete without ending human PIE.
	if (Session.IsValid() && bOwnedPie && !Session->IsOwnedPIEEnded())
	{
		return;
	}

	ActiveRun.State = PendingTerminalState;
	ActiveRun.FinalizedAtUtc = FDateTime::UtcNow();
	if (PendingTerminalState == ECortexReplayState::Error)
	{
		ActiveRun.ExecutionError = PendingTerminalResult;
	}
	else
	{
		ActiveRun.ExecutionError = FCortexCommandResult();
	}
	RunStore.SaveTerminal(ActiveRun);
	UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s finalized as %s"),
		*ServiceGuidToString(ActiveRun.Id), *FString(ServiceStateToString(ActiveRun.State)));

	bTerminalPublished = true;
	bFinalizing = false;
	bRunActive = false;
	SetRunState(PendingTerminalState);

	if (Session.IsValid())
	{
		Session->Shutdown();
		Session.Reset();
	}
}

FCortexReplayGuardDecision FCortexReplayService::FImpl::EvaluateGuard(const FCortexReplayEvent& Event)
{
	FCortexReplayGuardDecision Decision;

	// Validate the exact target before every guard decision.
	FCortexCommandResult TargetError;
	if (!Session.IsValid() || !Session->ValidateTarget(TargetError))
	{
		bInterruptionRequested = true;
		InterruptionResult = TargetError.bSuccess
			? ServiceError(CortexReplayErrorCodes::TargetUnavailable, TEXT("Replay target is unavailable"))
			: TargetError;
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = InterruptionResult;
		return Decision;
	}

	FCortexEditorPhysicalInputUIObservation Observation;
	if (Event.Guard.IsSet())
	{
		const FCortexReplayInteractionGuard& Guard = Event.Guard.GetValue();
		if (Guard.UICoverage == ECortexEditorUICoverage::Supported && Guard.UITarget.IsValid())
		{
			// Live normal-route evidence is sampled before the pose.
			Session->ObserveUI(Event.Input, *Guard.UITarget, Observation);
		}
	}

	FCortexEditorPhysicalInputPlayerPose ActualPose;
	if (!Session->ReadPlayerPose(ActualPose).bSuccess)
	{
		bInterruptionRequested = true;
		InterruptionResult = ServiceError(CortexReplayErrorCodes::TargetUnavailable,
			TEXT("Replay target pose could not be read"));
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = InterruptionResult;
		return Decision;
	}

	Decision = FCortexReplayGuardEvaluator::Evaluate(Event, ActualPose, Observation,
		Session->CanWaitForUI());
	return Decision;
}

FCortexCommandResult FCortexReplayService::FImpl::DispatchEvent(const FCortexReplayEvent& Event)
{
	if (!Session.IsValid())
	{
		return ServiceError(CortexReplayErrorCodes::TargetUnavailable, TEXT("Replay target is gone"));
	}
	return Session->Dispatch(Event.Input);
}

void FCortexReplayService::FImpl::OnCaptureEvent(const FCortexEditorPhysicalInputEvent& Event,
	double TimeSeconds, const FCortexEditorPhysicalInputCaptureContext& Context)
{
	if (!bCaptureActive || bFrozen)
	{
		return;
	}

	FCortexReplayEvent Recorded;
	Recorded.Sequence = CaptureEvents.Num();
	Recorded.TimeSeconds = CaptureEpochSeconds > 0.0
		? FMath::Max(0.0, TimeSeconds - CaptureEpochSeconds) : 0.0;
	Recorded.Input = Event;
	Recorded.CaptureContext.FrameNumber = Context.FrameNumber;
	Recorded.CaptureContext.WorldTimeSeconds = Context.WorldTimeSeconds;
	Recorded.CaptureContext.bWorldPaused = Context.bWorldPaused;
	Recorded.CaptureContext.bTargetOwnsPointerCapture = Context.bTargetOwnsPointerCapture;

	const bool bPress = !Event.bRepeat
		&& (Event.Kind == ECortexEditorPhysicalInputKind::KeyDown
			|| Event.Kind == ECortexEditorPhysicalInputKind::PointerDown
			|| Event.Kind == ECortexEditorPhysicalInputKind::DoubleClick);
	if (bPress)
	{
		FCortexReplayInteractionGuard Guard;
		Guard.ExpectedPose = Context.PressPose.IsSet() ? Context.PressPose.GetValue() : CaptureInitialPose;
		Guard.UICoverage = Context.UICoverage;
		Guard.UIUnavailableReason = Context.UIUnavailableReason;
		Guard.UITarget = Context.UITarget;
		Guard.ExpectedLocalPosition = Context.UILocalPosition;
		Recorded.Guard = Guard;
	}

	CaptureEvents.Add(MoveTemp(Recorded));
}

FCortexReplayService::FCortexReplayService(const FString& ProjectRoot)
	: Impl(MakeUnique<FImpl>(this, ProjectRoot))
{
	Impl->EnsureTicker();
}

FCortexReplayService::~FCortexReplayService()
{
	Shutdown();
}

FCortexCommandResult FCortexReplayService::Finalize(ECortexReplayState TerminalState,
	const FCortexCommandResult& ExecutionResult)
{
	FImpl& State = *Impl;
	if (!State.bFinalizing)
	{
		State.bFinalizing = true;
		State.bFrozen = true;
		State.PendingTerminalState = TerminalState;
		State.PendingTerminalResult = ExecutionResult;

		if (State.Session.IsValid())
		{
			// The cleanup contract applies to a bound original target. A run that never bound
			// (cancelled while still preparing) or whose target already disappeared has no live
			// owned input to neutralize, so a cleanup failure there must not rewrite an
			// intentional Cancelled/Interrupted result into an execution Error.
			FCortexCommandResult TargetError;
			const bool bTargetAvailable = State.Session->ValidateTarget(TargetError);

			const FCortexCommandResult Cleanup = State.Session->ReleaseHeldInputs();
			if (!Cleanup.bSuccess && bTargetAvailable
				&& State.PendingTerminalState != ECortexReplayState::Error)
			{
				State.PendingTerminalState = ECortexReplayState::Error;
				State.PendingTerminalResult = Cleanup;
			}
			if (State.bOwnedPie)
			{
				State.Session->EndOwnedPIE();
			}
		}
	}

	State.SetRunState(ECortexReplayState::Finalizing);
	State.EnsureTicker();
	State.TickFinalization();
	UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s finalizing as %s%s%s"),
		*ServiceGuidToString(State.ActiveRun.Id),
		*FString(ServiceStateToString(State.PendingTerminalState)),
		State.PendingTerminalResult.ErrorCode.IsEmpty() ? TEXT("") : TEXT(" "),
		State.PendingTerminalResult.ErrorCode.IsEmpty()
			? TEXT("") : *State.PendingTerminalResult.ErrorCode);
	return ServiceSuccess();
}

FCortexCommandResult FCortexReplayService::StartReplay(int32 Id, ECortexReplayOrigin Origin)
{
	FImpl& State = *Impl;
	auto Refuse = [Id](const FCortexCommandResult& Result) -> FCortexCommandResult
	{
		UE_LOG(LogCortexReplay, Log, TEXT("Replay start for recording %d refused: %s (%s)"),
			Id, *Result.ErrorCode, *Result.ErrorMessage);
		return Result;
	};
	if (State.bShutdown)
	{
		return Refuse(ServiceError(CortexReplayErrorCodes::InvalidOperation, TEXT("Replay service is shut down")));
	}
	if (State.bRunActive || State.bFinalizing || State.bCaptureActive)
	{
		return Refuse(ServiceError(CortexErrorCodes::EditorBusy, TEXT("Another replay operation owns the target")));
	}

	TSharedPtr<const FCortexReplaySnapshot> SnapshotPtr;
	const FCortexCommandResult Loaded = State.Library.Load(Id, Origin == ECortexReplayOrigin::AI, SnapshotPtr);
	if (!Loaded.bSuccess)
	{
		return Refuse(Loaded);
	}
	if (!SnapshotPtr.IsValid())
	{
		return Refuse(ServiceError(CortexReplayErrorCodes::InvalidRecording, TEXT("Recording snapshot is invalid")));
	}

	FCortexReplayRunRecord Record;
	Record.Id = FGuid::NewGuid();
	Record.RecordingId = Id;
	Record.Origin = Origin;
	Record.State = ECortexReplayState::Preparing;
	Record.EditorInstanceId = State.EditorInstanceId;
	Record.StartedAtUtc = FDateTime::UtcNow();
	Record.RecordingSnapshotSha256 = SnapshotPtr->RecordingSnapshotSha256;
	Record.InitialStateSha256 = SnapshotPtr->Metadata.InitialStateSha256;
	Record.InputsSha256 = SnapshotPtr->Metadata.InputsSha256;
	Record.TotalEvents = SnapshotPtr->Events.Num();
	Record.GuardCoverage = SnapshotPtr->Metadata.GuardCoverage;

	State.ActiveRun = Record;
	State.Snapshot = SnapshotPtr;
	State.Scheduler.Reset();
	State.bRunActive = true;
	State.bFinalizing = false;
	State.bTerminalPublished = false;
	State.bFrozen = false;
	State.bOwnedPie = true;
	State.bReadySeen = false;
	State.bCancellationRequested = false;
	State.bInterruptionRequested = false;
	State.PreparationResult = FCortexCommandResult();
	State.SetRunState(ECortexReplayState::Preparing);
	State.PrepareDeadline = FPlatformTime::Seconds() + ServicePreparationDeadlineSeconds;

	State.Session = MakeShared<FCortexEditorPhysicalInputSession>();

	FCortexReplayService::FImpl* RawState = &State;
	State.Session->SetInterruptionCallback(
		[RawState](const FCortexCommandResult& Interruption)
		{
			// Reentrant callbacks only record flags; no second tick drains a successor.
			RawState->bInterruptionRequested = true;
			RawState->InterruptionResult = Interruption;
		});

	const FCortexCommandResult Accepted = State.Session->BeginOwnedPIE(
		SnapshotPtr->Metadata.MapAssetPath,
		SnapshotPtr->Metadata.Prerequisites.LocalPlayerIndex,
		[RawState](const FCortexCommandResult& Ready)
		{
			RawState->bReadySeen = true;
			RawState->PreparationResult = Ready;
		});
	if (!Accepted.bSuccess)
	{
		// Acceptance failures still become this run's terminal Error result.
		State.bReadySeen = true;
		State.PreparationResult = Accepted;
	}

	State.EnsureTicker();

	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("run_id"), ServiceGuidToString(State.ActiveRun.Id));
	Data->SetNumberField(TEXT("recording_id"), Id);
	Data->SetStringField(TEXT("editor_instance_id"), State.EditorInstanceId);
	Data->SetStringField(TEXT("state"), ServiceStateToString(State.RunState));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexReplayService::GetRun(const FGuid& Id, bool bAIOnly) const
{
	const FImpl& State = *Impl;
	const bool bMatchesActive = State.ActiveRun.Id == Id
		&& (!bAIOnly || State.ActiveRun.Origin == ECortexReplayOrigin::AI);
	if (bMatchesActive && (State.bRunActive || State.bFinalizing || State.bTerminalPublished))
	{
		return FCortexCommandRouter::Success(BuildRunData(State.ActiveRun, State.bRunActive
			&& State.RunState == ECortexReplayState::Replaying, State.Scheduler.Get()));
	}

	FCortexReplayRunRecord Record;
	const ECortexReplayOrigin Origin = bAIOnly ? ECortexReplayOrigin::AI : ECortexReplayOrigin::Human;
	const FCortexCommandResult Loaded = State.RunStore.Load(Id, Origin, Record);
	if (!Loaded.bSuccess)
	{
		return Loaded;
	}
	return FCortexCommandRouter::Success(BuildRunData(Record, false, nullptr));
}

TSharedRef<FJsonObject> FCortexReplayService::BuildRunData(const FCortexReplayRunRecord& Record,
	bool bLive, const FCortexReplayScheduler* Scheduler)
{
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("run_id"), ServiceGuidToString(Record.Id));
	Data->SetNumberField(TEXT("recording_id"), Record.RecordingId);
	Data->SetStringField(TEXT("origin"), ServiceOriginToString(Record.Origin));
	Data->SetStringField(TEXT("editor_instance_id"), Record.EditorInstanceId);
	Data->SetStringField(TEXT("state"), ServiceStateToString(Record.State));
	Data->SetStringField(TEXT("guard_scope"), TEXT("press_only"));
	Data->SetObjectField(TEXT("guard_coverage"), ServiceGuardCoverageToJson(Record.GuardCoverage));
	Data->SetNumberField(TEXT("dispatched_events"), Record.DispatchedEvents);
	Data->SetNumberField(TEXT("total_events"), Record.TotalEvents);
	Data->SetNumberField(TEXT("authorized_wait_seconds"), Record.AuthorizedWaitSeconds);
	Data->SetStringField(TEXT("recording_snapshot_sha256"), Record.RecordingSnapshotSha256);
	Data->SetStringField(TEXT("initial_state_sha256"), Record.InitialStateSha256);
	Data->SetStringField(TEXT("inputs_sha256"), Record.InputsSha256);
	Data->SetStringField(TEXT("started_at_utc"), ServiceIsoUtc(Record.StartedAtUtc));
	if (Record.FinalizedAtUtc.GetTicks() > 0)
	{
		Data->SetField(TEXT("finalized_at_utc"),
			MakeShared<FJsonValueString>(ServiceIsoUtc(Record.FinalizedAtUtc)));
	}
	else
	{
		Data->SetField(TEXT("finalized_at_utc"), MakeShared<FJsonValueNull>());
	}

	if (Record.State == ECortexReplayState::Error && !Record.ExecutionError.ErrorCode.IsEmpty())
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), Record.ExecutionError.ErrorCode);
		Error->SetStringField(TEXT("message"), Record.ExecutionError.ErrorMessage);
		Data->SetObjectField(TEXT("execution_error"), Error);
	}
	else
	{
		Data->SetField(TEXT("execution_error"), MakeShared<FJsonValueNull>());
	}

	if (bLive && Scheduler != nullptr && Scheduler->GetWaitingSequence() != INDEX_NONE)
	{
		TSharedRef<FJsonObject> Waiting = MakeShared<FJsonObject>();
		Waiting->SetNumberField(TEXT("sequence"), Scheduler->GetWaitingSequence());
		Waiting->SetStringField(TEXT("reason"),
			FString(ServiceObservationStateToString(Scheduler->GetWaitReason())));
		Waiting->SetNumberField(TEXT("elapsed_seconds"), Scheduler->GetAuthorizedWaitSeconds());
		Waiting->SetNumberField(TEXT("remaining_event_seconds"),
			FMath::Max(0.0, 1.0 - Scheduler->GetAuthorizedWaitSeconds()));
		Waiting->SetNumberField(TEXT("remaining_run_seconds"),
			FMath::Max(0.0, 5.0 - Scheduler->GetAuthorizedWaitSeconds()));
		Data->SetObjectField(TEXT("waiting"), Waiting);
	}
	else
	{
		Data->SetField(TEXT("waiting"), MakeShared<FJsonValueNull>());
	}

	return Data;
}

FCortexCommandResult FCortexReplayService::CancelReplay(const FGuid& Id, bool bAIOnly)
{
	FImpl& State = *Impl;
	const bool bMatchesActive = State.ActiveRun.Id == Id
		&& (!bAIOnly || State.ActiveRun.Origin == ECortexReplayOrigin::AI);
	if (bMatchesActive && State.bRunActive)
	{
		State.bCancellationRequested = true;
		Finalize(ECortexReplayState::Cancelled, ServiceSuccess());
		return FCortexCommandRouter::Success(BuildRunData(State.ActiveRun, true, State.Scheduler.Get()));
	}
	if (bMatchesActive && State.bTerminalPublished)
	{
		return FCortexCommandRouter::Success(BuildRunData(State.ActiveRun, false, nullptr));
	}

	FCortexReplayRunRecord Record;
	const ECortexReplayOrigin Origin = bAIOnly ? ECortexReplayOrigin::AI : ECortexReplayOrigin::Human;
	const FCortexCommandResult Loaded = State.RunStore.Load(Id, Origin, Record);
	if (!Loaded.bSuccess)
	{
		return Loaded;
	}
	return FCortexCommandRouter::Success(BuildRunData(Record, false, nullptr));
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
	if (!State.bRunActive && !State.bCaptureActive)
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
	}
	else
	{
		Data->SetStringField(TEXT("kind"), TEXT("capture"));
		Data->SetStringField(TEXT("origin"), State.bBorrowedCapture ? TEXT("human") : TEXT("ai"));
		Data->SetNumberField(TEXT("recording_id"), State.CaptureRecordingId);
		Data->SetStringField(TEXT("state"), ServiceStateToString(ECortexReplayState::Recording));
	}
	return FCortexCommandRouter::Success(Data);
}

bool FCortexReplayService::IsRecordInUse(int32 Id) const
{
	const FImpl& State = *Impl;
	if (State.bCaptureActive && State.CaptureRecordingId == Id)
	{
		return true;
	}
	if ((State.bRunActive || State.bFinalizing) && State.ActiveRun.RecordingId == Id)
	{
		return true;
	}
	return false;
}

FCortexCommandResult FCortexReplayService::EnumerateHumanCaptureTargets(
	TArray<FCortexReplayCaptureTargetChoice>& Out) const
{
	Out.Reset();
	if (!GEngine)
	{
		return ServiceError(CortexReplayErrorCodes::TargetUnavailable,
			TEXT("No engine world contexts are available"));
	}

	int32 Resolved = 0;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (Context.WorldType != EWorldType::PIE || World == nullptr)
		{
			continue;
		}
		// Exclude non-rendered/unready candidates rather than selecting the first.
		if (World->GetGameViewport() == nullptr)
		{
			continue;
		}
		ULocalPlayer* LocalPlayer = World->GetFirstLocalPlayerFromController();
		if (LocalPlayer == nullptr)
		{
			continue;
		}
		if (World->GetFirstPlayerController() == nullptr)
		{
			continue;
		}

		FCortexReplayCaptureTargetChoice Choice;
		Choice.World = World;
		Choice.LocalPlayerIndex = 0;
		Choice.MapAssetPath = UWorld::RemovePIEPrefix(World->GetPackage()->GetName());
		Choice.ViewportLabel = FString::Printf(TEXT("%s [PIE]"), *Choice.MapAssetPath);
		Out.Add(MoveTemp(Choice));
		++Resolved;
	}

	if (Resolved == 0)
	{
		return ServiceError(CortexReplayErrorCodes::TargetUnavailable,
			TEXT("No rendered PIE local-player/viewport candidate could be resolved"));
	}
	return ServiceSuccess();
}

FCortexCommandResult FCortexReplayService::StartCaptureAtTarget(UWorld& World, int32 LocalPlayerIndex)
{
	FImpl& State = *Impl;
	if (State.bRunActive || State.bFinalizing || State.bCaptureActive)
	{
		return ServiceError(CortexErrorCodes::EditorBusy, TEXT("Another replay operation owns the target"));
	}

	State.Session = MakeShared<FCortexEditorPhysicalInputSession>();
	const FCortexCommandResult Bound = State.Session->BindTarget(World, LocalPlayerIndex);
	if (!Bound.bSuccess)
	{
		State.Session.Reset();
		return Bound;
	}

	int32 ReservedId = 0;
	const FCortexCommandResult Reserved = State.Library.ReserveId(ReservedId);
	if (!Reserved.bSuccess)
	{
		State.Session->Shutdown();
		State.Session.Reset();
		return Reserved;
	}

	State.CaptureRecordingId = ReservedId;
	State.CaptureTargetInfo = State.Session->GetTargetInfo();
	State.CaptureMapAssetPath = State.CaptureTargetInfo.MapAssetPath;
	State.CaptureEvents.Reset();

	FCortexEditorPhysicalInputPlayerPose Pose;
	if (State.Session->ReadPlayerPose(Pose).bSuccess)
	{
		State.CaptureInitialPose = Pose;
	}
	else
	{
		State.CaptureInitialPose = FCortexEditorPhysicalInputPlayerPose();
	}

	FCortexReplayService::FImpl* RawState = &State;
	const FCortexCommandResult Armed = State.Session->SetCaptureCallback(
		[RawState](const FCortexEditorPhysicalInputEvent& Event, double TimeSeconds,
			const FCortexEditorPhysicalInputCaptureContext& Context)
		{
			RawState->OnCaptureEvent(Event, TimeSeconds, Context);
		});
	if (!Armed.bSuccess)
	{
		State.Session->Shutdown();
		State.Session.Reset();
		return Armed;
	}

	State.bCaptureActive = true;
	State.bBorrowedCapture = true;
	State.bFrozen = false;
	State.CaptureEpochSeconds = FPlatformTime::Seconds();
	State.EnsureTicker();
	return ServiceSuccess();
}

FCortexCommandResult FCortexReplayService::StartCapture(const FString& SavedEditorMapAssetPath)
{
	FImpl& State = *Impl;
	if (State.bRunActive || State.bFinalizing || State.bCaptureActive)
	{
		return ServiceError(CortexErrorCodes::EditorBusy, TEXT("Another replay operation owns the target"));
	}

	int32 ReservedId = 0;
	const FCortexCommandResult Reserved = State.Library.ReserveId(ReservedId);
	if (!Reserved.bSuccess)
	{
		return Reserved;
	}

	State.Session = MakeShared<FCortexEditorPhysicalInputSession>();
	State.CaptureRecordingId = ReservedId;
	State.CaptureEvents.Reset();
	State.bBorrowedCapture = false;
	State.bFrozen = false;

	FCortexReplayService::FImpl* RawState = &State;
	const FCortexCommandResult Armed = State.Session->BeginOwnedPIE(SavedEditorMapAssetPath, 0,
		[RawState](const FCortexCommandResult& Ready)
		{
			// Owned capture arms as soon as the target is ready.
			if (Ready.bSuccess && RawState->Session.IsValid())
			{
				FCortexEditorPhysicalInputPlayerPose Pose;
				RawState->Session->ReadPlayerPose(Pose);
				RawState->Session->SetCaptureCallback(
					[RawState](const FCortexEditorPhysicalInputEvent& Event, double TimeSeconds,
						const FCortexEditorPhysicalInputCaptureContext& Context)
					{
						RawState->OnCaptureEvent(Event, TimeSeconds, Context);
					});
				RawState->CaptureTargetInfo = RawState->Session->GetTargetInfo();
				RawState->CaptureMapAssetPath = RawState->CaptureTargetInfo.MapAssetPath;
				RawState->CaptureInitialPose = Pose;
				RawState->bCaptureActive = true;
				RawState->CaptureEpochSeconds = FPlatformTime::Seconds();
			}
		});
	if (!Armed.bSuccess)
	{
		State.Session->Shutdown();
		State.Session.Reset();
		return Armed;
	}

	State.EnsureTicker();
	return ServiceSuccess();
}

FCortexCommandResult FCortexReplayService::StopCapture(bool bAbnormal)
{
	FImpl& State = *Impl;
	if (!State.bCaptureActive)
	{
		return ServiceError(CortexReplayErrorCodes::InvalidOperation, TEXT("No capture is active"));
	}

	State.bCaptureActive = false;
	State.bFrozen = true;
	if (State.Session.IsValid())
	{
		State.Session->ReleaseHeldInputs();
		State.Session->Shutdown();
		State.Session.Reset();
	}

	if (bAbnormal)
	{
		// Abnormal capture termination stays incomplete/error and publishes no partial record.
		State.CaptureEvents.Reset();
		return ServiceSuccess();
	}

	FCortexReplaySnapshot Snapshot;
	Snapshot.InitialState.SchemaVersion = 1;
	Snapshot.InitialState.RecordingId = State.CaptureRecordingId;
	Snapshot.InitialState.PawnClassPath = State.CaptureTargetInfo.PawnClassPath;
	Snapshot.InitialState.Pose = State.CaptureInitialPose;
	Snapshot.Events = State.CaptureEvents;

	double FinalTime = 0.0;
	for (const FCortexReplayEvent& Event : Snapshot.Events)
	{
		FinalTime = FMath::Max(FinalTime, Event.TimeSeconds);
		if (Event.Guard.IsSet())
		{
			++Snapshot.Metadata.GuardCoverage.PosePresses;
			switch (Event.Guard->UICoverage)
			{
			case ECortexEditorUICoverage::Supported:
				++Snapshot.Metadata.GuardCoverage.UISupportedPresses;
				break;
			case ECortexEditorUICoverage::Unavailable:
				++Snapshot.Metadata.GuardCoverage.UIUnavailablePresses;
				break;
			default:
				++Snapshot.Metadata.GuardCoverage.UINotApplicablePresses;
				break;
			}
		}
	}

	Snapshot.Metadata.SchemaVersion = 1;
	Snapshot.Metadata.RecordingId = State.CaptureRecordingId;
	Snapshot.Metadata.Name = FString::Printf(TEXT("Capture %d"), State.CaptureRecordingId);
	Snapshot.Metadata.Description.Reset();
	Snapshot.Metadata.MapAssetPath = State.CaptureMapAssetPath;
	Snapshot.Metadata.EngineVersion = FEngineVersion::Current().ToString();
	Snapshot.Metadata.PluginVersion = TEXT("0.4.0");
	Snapshot.Metadata.CreatedAtUtc = FDateTime::UtcNow();
	Snapshot.Metadata.DurationSeconds = FinalTime;
	Snapshot.Metadata.bAIEnabled = false;
	Snapshot.Metadata.bComplete = true;
	Snapshot.Metadata.Prerequisites = State.CaptureTargetInfo;

	State.CaptureEvents.Reset();
	return State.Library.Publish(Snapshot);
}

FCortexCommandResult FCortexReplayService::GetRecording(int32 Id, bool bAIOnly) const
{
	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	const FCortexCommandResult Loaded = Impl->Library.Load(Id, bAIOnly, Snapshot);
	if (!Loaded.bSuccess)
	{
		return Loaded;
	}

	TSharedRef<FJsonObject> Data = ServiceMetadataToJson(Snapshot->Metadata);
	TSharedRef<FJsonObject> InitialState = MakeShared<FJsonObject>();
	InitialState->SetNumberField(TEXT("schema_version"), Snapshot->InitialState.SchemaVersion);
	InitialState->SetNumberField(TEXT("recording_id"), Snapshot->InitialState.RecordingId);
	InitialState->SetStringField(TEXT("pawn_class_path"), Snapshot->InitialState.PawnClassPath);

	const FCortexEditorPhysicalInputPlayerPose& Pose = Snapshot->InitialState.Pose;
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
	Data->SetStringField(TEXT("recording_snapshot_sha256"), Snapshot->RecordingSnapshotSha256);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexReplayService::ListRecordings(int32 AfterId, int32 PageSize) const
{
	TArray<FCortexReplayMetadata> Metadata;
	bool bHasMore = false;
	const FCortexCommandResult Listed = Impl->Library.ListPage(false, AfterId, PageSize, Metadata, bHasMore);
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
		Data->SetNumberField(TEXT("next_after_recording_id"),
			Metadata.Last().RecordingId);
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
			BuildRunData(Impl->ActiveRun, true, Impl->Scheduler.Get())));
	}
	else
	{
		Data->SetField(TEXT("active_ai_run"), MakeShared<FJsonValueNull>());
	}
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexReplayService::ListHumanRecordings(
	TArray<FCortexReplayMetadata>& Out) const
{
	return Impl->Library.List(false, Out);
}

FCortexCommandResult FCortexReplayService::SaveMetadata(int32 Id, const FString& Name,
	const FString& Description, bool bAIEnabled)
{
	FImpl& State = *Impl;
	const FCortexCommandResult Saved = State.Library.SaveMetadata(Id, Name, Description, bAIEnabled);
	if (!Saved.bSuccess)
	{
		return Saved;
	}

	// Revocation cancels only a still-active AI run of that recording.
	if (!bAIEnabled && State.bRunActive && !State.bFinalizing
		&& State.ActiveRun.Origin == ECortexReplayOrigin::AI && State.ActiveRun.RecordingId == Id)
	{
		State.bCancellationRequested = true;
		Finalize(ECortexReplayState::Cancelled,
			ServiceError(CortexReplayErrorCodes::PermissionDenied,
				TEXT("Replay permission was revoked")));
	}
	return Saved;
}

FCortexCommandResult FCortexReplayService::DeleteRecording(int32 Id)
{
	if (IsRecordInUse(Id))
	{
		return ServiceError(CortexErrorCodes::EditorBusy, TEXT("Recording is in use"));
	}
	return Impl->Library.Delete(Id);
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

	if (State.TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(State.TickerHandle);
		State.TickerHandle.Reset();
	}
	if (State.Session.IsValid())
	{
		State.Session->Shutdown();
		State.Session.Reset();
	}
	State.bCaptureActive = false;
	State.bRunActive = false;
	State.bFinalizing = false;
}
