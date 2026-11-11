#include "CortexReplayServiceImpl.h"

#include "CortexReplayErrorCodes.h"
#include "CortexReplayFormat.h"

#include "Dom/JsonValue.h"
#include "Framework/Commands/InputChord.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Kismet2/DebuggerCommands.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"

using namespace CortexReplayServicePrivate;

namespace
{
/**
 * The immutable recording provenance version comes from the actual loaded plugin descriptor, so
 * it can never drift from the shipped UnrealCortex.uplugin as a second hard-coded string. An
 * unresolved descriptor yields an empty version and publication validation then rejects it.
 */
FString ResolveCortexPluginVersion()
{
	IPluginManager& PluginManager = IPluginManager::Get();
	if (const TSharedPtr<IPlugin> Plugin = PluginManager.FindPlugin(FString(TEXT("UnrealCortex"))))
	{
		return Plugin->GetDescriptor().VersionName;
	}
	return FString();
}

/**
 * True when the capture's bound target is gone because the whole PIE session ended: the bound world
 * is no longer a running PIE world (or the binding was released). This is the human ending the
 * session rather than an exact target-identity loss (pawn replaced, class changed, local player
 * re-bound), which must keep the ordinary fault-and-discard behaviour.
 */
bool IsCaptureTargetSessionEnded(const FCortexEditorPhysicalInputSession& Session)
{
	const UWorld* BoundWorld = Session.GetTargetBinding().World.Get();
	return BoundWorld == nullptr || BoundWorld->WorldType != EWorldType::PIE;
}

/** The key/button presses still held at the end of a captured stream (never matched by a release). */
TSet<FKey> CollectHeldCaptureKeys(const TArray<FCortexReplayEvent>& Events)
{
	TSet<FKey> Held;
	for (const FCortexReplayEvent& Event : Events)
	{
		switch (Event.Input.Kind)
		{
		case ECortexEditorPhysicalInputKind::KeyDown:
		case ECortexEditorPhysicalInputKind::PointerDown:
		case ECortexEditorPhysicalInputKind::DoubleClick:
			Held.Add(Event.Input.Key);
			break;
		case ECortexEditorPhysicalInputKind::KeyUp:
		case ECortexEditorPhysicalInputKind::PointerUp:
			Held.Remove(Event.Input.Key);
			break;
		default:
			break;
		}
	}
	return Held;
}


/** Stable, sorted names of the presses still held at the end of the captured stream. */
TArray<FString> CollectedHeldInputNames(const TArray<FCortexReplayEvent>& Events)
{
	const TSet<FKey> Held = CollectHeldCaptureKeys(Events);
	TArray<FString> Names;
	Names.Reserve(Held.Num());
	for (const FKey& Key : Held)
	{
		Names.Add(Key.GetFName().ToString());
	}
	Names.Sort();
	return Names;
}

/**
 * True when a capture publication error can be cleared by the environment, so the frozen snapshot is
 * retained and republished: a storage/authoring-lock failure or an occupied recording path
 * (STORAGE_FAILURE / EDITOR_BUSY). Every other verdict is about the stream itself (an incomplete
 * stream, a stream the library's format validation rejects): republishing the identical stream can
 * never change it, so the operation releases instead of retrying forever.
 */
bool IsTransientCapturePublicationError(const FString& Code)
{
	return Code == CortexReplayErrorCodes::StorageFailure
		|| Code == CortexReplayErrorCodes::EditorBusy;
}
}

const TCHAR* FCortexReplayService::FImpl::CapturePhaseToString(ECapturePhase Phase)
{
	switch (Phase)
	{
	case ECapturePhase::None: return TEXT("None");
	case ECapturePhase::Preparing: return TEXT("Preparing");
	case ECapturePhase::Recording: return TEXT("Recording");
	case ECapturePhase::Finalizing: return TEXT("Finalizing");
	default: return TEXT("Unknown");
	}
}

void FCortexReplayService::FImpl::ReleaseCaptureFrameObserver()
{
	if (!CaptureFrameObserver.IsValid())
	{
		return;
	}
	// Uninstall detaches from the exact session and clears that link, so a later destructor call is
	// harmless even after the session object is gone.
	CaptureFrameObserver->Uninstall();
	CaptureFrameObserver.Reset();
}

void FCortexReplayService::FImpl::MarkCaptureFaulted(const FCortexCommandResult& Result)
{
	bCaptureFaulted = true;
	CaptureFaultResult = Result.bSuccess
		? ServiceError(CortexReplayErrorCodes::TargetUnavailable, TEXT("Capture target was lost"))
		: Result;
	UE_LOG(LogCortexReplay, Display, TEXT("Capture %d faulted: %s (%s)"),
		CaptureRecordingId, *CaptureFaultResult.ErrorCode, *CaptureFaultResult.ErrorMessage);
	if (CapturePhase == ECapturePhase::Finalizing)
	{
		// A fault during teardown withholds the pending publication.
		bCapturePublishOnComplete = false;
	}
}

void FCortexReplayService::FImpl::HandleCaptureTargetLoss(const FCortexCommandResult& TargetError)
{
	// A whole session/world loss is the human ending the session rather than an exact target-identity
	// loss: it is treated as a normal stop whose publication still requires a publishable stream.
	if (Session.IsValid() && IsCaptureTargetSessionEnded(*Session))
	{
		// The open frame is part of the stream (it consumed its input before the session ended);
		// the stop-play key press that ended the session is not an input to replay.
		SealCaptureFrames();
		DropTrailingStopChordPress();
		CaptureStreamHeldInputs = CollectedHeldInputNames(CaptureEvents);
		bCaptureStreamBalanced = CaptureStreamHeldInputs.IsEmpty();
		// Bounded diagnostic: the exact held identities are otherwise unrecoverable once the stream
		// is withheld, and the range is the supported key domain.
		UE_LOG(LogCortexReplay, Display,
			TEXT("Capture %d target/session ended externally (balanced=%d, events=%d, held=[%s]); stopping to publish"),
			CaptureRecordingId, bCaptureStreamBalanced ? 1 : 0, CaptureEvents.Num(),
			*FString::Join(CaptureStreamHeldInputs, TEXT(",")));
		BeginCaptureFinalization(true);
	}
	else
	{
		MarkCaptureFaulted(TargetError);
		BeginCaptureFinalization(false);
	}
	TickCapture();
}

void FCortexReplayService::FImpl::BeginCaptureFinalization(bool bPublish)
{
	if (CapturePhase == ECapturePhase::None || CapturePhase == ECapturePhase::Finalizing)
	{
		return;
	}
	if (CapturePhase == ECapturePhase::Recording)
	{
		SealCaptureFrames();
	}
	CapturePhase = ECapturePhase::Finalizing;
	bCapturePublishOnComplete = bPublish;
	bFrozen = true;
	UE_LOG(LogCortexReplay, Display, TEXT("Capture %d finalization entered (publish=%d)"),
		CaptureRecordingId, bPublish ? 1 : 0);
	if (Session.IsValid())
	{
		// Freeze dispatch and detach capture; only a matching owned session is ended.
		Session->ReleaseHeldInputs();
		if (bOwnedCapture)
		{
			Session->EndOwnedPIE();
		}
	}
}

void FCortexReplayService::FImpl::RetainCaptureForPublicationRetry(
	const FCortexCommandResult& PublishResult)
{
	// Keep the captured events, recording id and Finalizing phase so the publication failure stays
	// queryable and can be retried instead of silently completing. The failure is retained
	// separately from the interruption faults so the retry path is never suppressed.
	bCapturePublicationFailed = true;
	CapturePublicationError = PublishResult;
	CapturePhase = ECapturePhase::Finalizing;
	bCapturePublishOnComplete = true;
	bFrozen = true;
	LastCapturePublishAttemptSeconds = FPlatformTime::Seconds();
}

void FCortexReplayService::FImpl::ClearCapturePublicationFailure()
{
	bCapturePublicationFailed = false;
	CapturePublicationError = FCortexCommandResult();
}

TSharedRef<FJsonObject> FCortexReplayService::FImpl::BuildLastCaptureOutcomeJson(bool bPublished,
	const FCortexCommandResult* PublicationError) const
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("kind"), TEXT("capture"));
	// Captures are always human-initiated; there is no AI capture origin. Ownership (borrowed vs
	// owned) is live-only operation state and is deliberately not duplicated into the summary.
	Object->SetStringField(TEXT("origin"), TEXT("human"));
	Object->SetNumberField(TEXT("recording_id"), CaptureRecordingId);
	Object->SetBoolField(TEXT("published"), bPublished);
	Object->SetBoolField(TEXT("faulted"), bCaptureFaulted);

	if (PublicationError != nullptr && !PublicationError->bSuccess)
	{
		Object->SetBoolField(TEXT("publication_failed"), true);
		const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), PublicationError->ErrorCode);
		Error->SetStringField(TEXT("message"), PublicationError->ErrorMessage);
		if (CaptureStreamHeldInputs.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> HeldValues;
			HeldValues.Reserve(CaptureStreamHeldInputs.Num());
			for (const FString& Name : CaptureStreamHeldInputs)
			{
				HeldValues.Add(MakeShared<FJsonValueString>(Name));
			}
			Error->SetArrayField(TEXT("held_inputs"), MoveTemp(HeldValues));
		}
		Object->SetObjectField(TEXT("publication_error"), Error);
	}
	else
	{
		Object->SetBoolField(TEXT("publication_failed"), false);
		Object->SetField(TEXT("publication_error"), MakeShared<FJsonValueNull>());
	}
	Object->SetField(TEXT("fault_error"), MakeShared<FJsonValueNull>());
	if (bCaptureFaulted && CaptureFaultResult.ErrorCode.IsEmpty() == false)
	{
		const TSharedRef<FJsonObject> Fault = MakeShared<FJsonObject>();
		Fault->SetStringField(TEXT("code"), CaptureFaultResult.ErrorCode);
		Fault->SetStringField(TEXT("message"), CaptureFaultResult.ErrorMessage);
		Object->SetObjectField(TEXT("fault_error"), Fault);
	}
	return Object;
}

void FCortexReplayService::FImpl::StoreLastCaptureOutcome(bool bPublished,
	const FCortexCommandResult* PublicationError)
{
	LastCaptureOutcome = BuildLastCaptureOutcomeJson(bPublished, PublicationError);
}

void FCortexReplayService::FImpl::ClearLastCaptureOutcome()
{
	LastCaptureOutcome.Reset();
}

void FCortexReplayService::FImpl::TickCapture()
{
	if (CapturePhase == ECapturePhase::None)
	{
		return;
	}

	if (CapturePhase == ECapturePhase::Preparing)
	{
		if (bCaptureFaulted)
		{
			BeginCaptureFinalization(false);
			TickCapture();
		}
		return;
	}

	if (CapturePhase == ECapturePhase::Recording)
	{
		if (bCaptureFaulted)
		{
			BeginCaptureFinalization(false);
			TickCapture();
			return;
		}
		// Target destruction is abnormal capture termination, never a complete recording. A whole
		// session/world loss (the human ended PIE without pressing Stop) is classified separately.
		if (Session.IsValid())
		{
			FCortexCommandResult TargetError;
			if (!Session->ValidateTarget(TargetError))
			{
				HandleCaptureTargetLoss(TargetError);
			}
		}
		return;
	}

	// Finalizing: owned captures retain their session until the matching teardown is observed.
	if (Session.IsValid() && bOwnedCapture && !bUnloading && !Session->IsOwnedPIEEnded())
	{
		return;
	}
	// A failed publication is retained and retried at most once per second.
	if (bCapturePublishOnComplete && !bCaptureFaulted && !bUnloading
		&& LastCapturePublishAttemptSeconds > 0.0
		&& FPlatformTime::Seconds() - LastCapturePublishAttemptSeconds < 1.0)
	{
		return;
	}
	CompleteCaptureFinalization();
}

void FCortexReplayService::FImpl::CompleteCaptureFinalization()
{
	const bool bShouldPublish = bCapturePublishOnComplete && !bCaptureFaulted;
	// The actual publication outcome: a permanent failure is not a publication.
	bool bCapturePublished = false;
	if (bShouldPublish)
	{
		LastCapturePublishAttemptSeconds = FPlatformTime::Seconds();
		const FCortexCommandResult PublishResult = PublishCaptureSnapshot();
		if (!PublishResult.bSuccess)
		{
			if (IsTransientCapturePublicationError(PublishResult.ErrorCode) && !bUnloading)
			{
				// Transient (storage/authoring-lock) failure: the ownership, recording id and captured
				// events are retained and the publication is retried at most once per second.
				RetainCaptureForPublicationRetry(PublishResult);
				UE_LOG(LogCortexReplay, Display, TEXT("Capture %d publication failed: %s (%s); retained for retry"),
					CaptureRecordingId, *PublishResult.ErrorCode, *PublishResult.ErrorMessage);
				return;
			}
			// Immutable for this frozen stream: republishing can never change the outcome. Retain the
			// explicit terminal failure for the human window, then release the operation instead of
			// looping forever and holding the capture lock.
			UE_LOG(LogCortexReplay, Display,
				TEXT("Capture %d publication permanently failed: %s (%s); releasing the operation"),
				CaptureRecordingId, *PublishResult.ErrorCode, *PublishResult.ErrorMessage);
			StoreLastCaptureOutcome(/*bPublished=*/false, &PublishResult);
		}
		else
		{
			ClearCapturePublicationFailure();
			StoreLastCaptureOutcome(/*bPublished=*/true, nullptr);
			bCapturePublished = true;
			UE_LOG(LogCortexReplay, Display, TEXT("Capture %d publication succeeded"), CaptureRecordingId);
		}
	}
	else if (bCaptureFaulted)
	{
		// A faulted capture is withheld, and its reason must survive the reset: it is the only
		// explanation the human gets after the operation returns to idle.
		const FCortexCommandResult* Fault = CaptureFaultResult.ErrorCode.IsEmpty()
			? nullptr : &CaptureFaultResult;
		StoreLastCaptureOutcome(/*bPublished=*/false, Fault);
	}
	ReleaseCaptureFrameObserver();
	FCortexEditorPhysicalInputSession::ShutdownAndRetainUntilOwnedTeardown(MoveTemp(Session));
	const int32 CompletedRecordingId = CaptureRecordingId;
	ResetCapture();
	UE_LOG(LogCortexReplay, Display, TEXT("Capture %d finalized (published=%d)"),
		CompletedRecordingId, bCapturePublished ? 1 : 0);
	if (bShutdown)
	{
		DetachTickerAndSession();
	}
}

void FCortexReplayService::FImpl::ResetCapture()
{
	CapturePhase = ECapturePhase::None;
	bOwnedCapture = false;
	bCaptureFaulted = false;
	bCapturePublishOnComplete = false;
	CaptureFaultResult = FCortexCommandResult();
	ClearCapturePublicationFailure();
	bCaptureStreamBalanced = true;
	CaptureStreamHeldInputs.Reset();
	CaptureRecordingId = 0;
	CaptureMapAssetPath.Reset();
	CaptureTargetInfo = FCortexEditorPhysicalInputTargetInfo();
	CaptureInitialPose = FCortexEditorPhysicalInputPlayerPose();
	CaptureEvents.Reset();
	CaptureFrames.Reset();
	CaptureOpenFrame.Reset();
	CaptureNextFirstSequence = 0;
	CaptureViewport = FCortexReplayViewportState();
	ReleaseCaptureFrameObserver();
	CaptureThrottle.Release();
	LastCapturePublishAttemptSeconds = 0.0;
	bFrozen = false;
}

FCortexCommandResult FCortexReplayService::FImpl::PublishCaptureSnapshot()
{
	// An incomplete stream (an external session end that left a held key/button unreleased) is never
	// published as a playable recording. The returned error is retained and reported exactly like a
	// publication validation failure, and names the held identities so the reason is actionable.
	if (!bCaptureStreamBalanced)
	{
		const FString HeldDetail = CaptureStreamHeldInputs.Num() > 0
			? FString::Printf(TEXT(" (%s)"), *FString::Join(CaptureStreamHeldInputs, TEXT(", ")))
			: FString();
		return ServiceError(CortexReplayErrorCodes::IncompleteRecording,
			FString::Printf(
				TEXT("The capture ended with an unreleased held key or button%s and is not publishable"),
				*HeldDetail));
	}

	FCortexReplaySnapshot CaptureSnapshot;
	CaptureSnapshot.InitialState.SchemaVersion = CortexReplayFormat::RecordingFormatVersion;
	CaptureSnapshot.InitialState.RecordingId = CaptureRecordingId;
	CaptureSnapshot.InitialState.PawnClassPath = CaptureTargetInfo.PawnClassPath;
	CaptureSnapshot.InitialState.Pose = CaptureInitialPose;
	CaptureSnapshot.InitialState.Viewport = CaptureViewport;
	CaptureSnapshot.Events = CaptureEvents;
	CaptureSnapshot.Frames = CaptureFrames;

	FCortexReplayMetadata& Metadata = CaptureSnapshot.Metadata;
	Metadata.SchemaVersion = CortexReplayFormat::RecordingFormatVersion;
	Metadata.RecordingId = CaptureRecordingId;
	Metadata.Name = FString::Printf(TEXT("Capture %d"), CaptureRecordingId);
	Metadata.Description.Reset();
	Metadata.MapAssetPath = CaptureMapAssetPath;
	Metadata.EngineVersion = FEngineVersion::Current().ToString();
	Metadata.PluginVersion = ResolveCortexPluginVersion();
	Metadata.CreatedAtUtc = FDateTime::UtcNow();
	// The recorded duration is the span the replay paces: the sum of the recorded frame deltas.
	Metadata.DurationSeconds = CortexReplayFormat::SumFrameDeltaSeconds(CaptureSnapshot.Frames);
	Metadata.GuardCoverage = CortexReplayFormat::ComputeGuardCoverage(CaptureSnapshot.Events);
	Metadata.Timing.FrameCount = CaptureSnapshot.Frames.Num();
	Metadata.bAIEnabled = false;
	Metadata.bComplete = true;
	Metadata.Prerequisites = CaptureTargetInfo;

	return Library.Publish(CaptureSnapshot);
}

void FCortexReplayService::FImpl::OnCaptureInterruption(uint64 Generation,
	const FCortexCommandResult& Result)
{
	if (Generation != CaptureOperationGeneration || CapturePhase == ECapturePhase::None)
	{
		return;
	}
	// An interruption raised while the whole session/world is gone is an external session end and
	// publishes like a normal stop; any other interruption is an input-level fault.
	if (CapturePhase == ECapturePhase::Recording && Session.IsValid()
		&& IsCaptureTargetSessionEnded(*Session))
	{
		HandleCaptureTargetLoss(Result);
		return;
	}
	MarkCaptureFaulted(Result);
}

void FCortexReplayService::FImpl::OnOwnedCaptureReady(uint64 Generation,
	const FCortexCommandResult& Ready)
{
	if (Generation != CaptureOperationGeneration)
	{
		UE_LOG(LogCortexReplay, Display,
			TEXT("Capture readiness callback ignored: stale generation (current %llu, callback %llu)"),
			CaptureOperationGeneration, Generation);
		return;
	}
	if (CapturePhase != ECapturePhase::Preparing)
	{
		UE_LOG(LogCortexReplay, Display,
			TEXT("Capture %d readiness callback ignored: phase is %s, not Preparing"),
			CaptureRecordingId, *FString(CapturePhaseToString(CapturePhase)));
		return;
	}
	if (!Session.IsValid())
	{
		UE_LOG(LogCortexReplay, Display,
			TEXT("Capture %d readiness callback ignored: the capture session is missing"),
			CaptureRecordingId);
		return;
	}
	if (!Ready.bSuccess)
	{
		UE_LOG(LogCortexReplay, Display, TEXT("Capture %d preparation failed: %s (%s)"),
			CaptureRecordingId, *Ready.ErrorCode, *Ready.ErrorMessage);
		MarkCaptureFaulted(Ready);
		return;
	}
	UE_LOG(LogCortexReplay, Display, TEXT("Capture %d preparation ready"), CaptureRecordingId);

	FCortexEditorPhysicalInputPlayerPose Pose;
	const FCortexCommandResult PoseResult = Session->ReadPlayerPose(Pose);
	if (!PoseResult.bSuccess)
	{
		UE_LOG(LogCortexReplay, Display, TEXT("Capture %d preparation pose read failed: %s (%s)"),
			CaptureRecordingId, *PoseResult.ErrorCode, *PoseResult.ErrorMessage);
		MarkCaptureFaulted(PoseResult);
		return;
	}

	bool bViewportMouseCapture = false;
	bool bViewportHighPrecision = false;
	const FCortexCommandResult ViewportResult =
		Session->ReadViewportInputState(bViewportMouseCapture, bViewportHighPrecision);
	if (!ViewportResult.bSuccess)
	{
		UE_LOG(LogCortexReplay, Display, TEXT("Capture %d viewport state read failed: %s (%s)"),
			CaptureRecordingId, *ViewportResult.ErrorCode, *ViewportResult.ErrorMessage);
		MarkCaptureFaulted(ViewportResult);
		return;
	}

	FCortexReplayService::FImpl* RawState = this;
	const FCortexCommandResult Armed = Session->SetCaptureCallback(
		[RawState](const FCortexEditorPhysicalInputEvent& Event, double TimeSeconds,
			const FCortexEditorPhysicalInputCaptureContext& Context)
		{
			RawState->OnCaptureEvent(Event, TimeSeconds, Context);
		});
	if (!Armed.bSuccess)
	{
		// A rejected neutral-state arming check is abnormal termination, never Recording.
		UE_LOG(LogCortexReplay, Display, TEXT("Capture %d arming rejected: %s (%s)"),
			CaptureRecordingId, *Armed.ErrorCode, *Armed.ErrorMessage);
		MarkCaptureFaulted(Armed);
		return;
	}

	CaptureTargetInfo = Session->GetTargetInfo();
	CaptureMapAssetPath = CaptureTargetInfo.MapAssetPath;
	CaptureInitialPose = Pose;
	CaptureViewport.bMouseCapture = bViewportMouseCapture;
	CaptureViewport.bHighPrecision = bViewportHighPrecision;
	CapturePhase = ECapturePhase::Recording;
	UE_LOG(LogCortexReplay, Display, TEXT("Capture %d entered Recording (epoch established)"),
		CaptureRecordingId);
}

void FCortexReplayService::FImpl::OnCaptureInputBoundary(uint64 Generation,
	const FCortexEditorEngineFrameRecord& Boundary)
{
	// The boundary of the epoch frame precedes the epoch (phase is still Preparing) and is ignored,
	// so the first stamped frame is the first boundary after the epoch.
	if (Generation != CaptureOperationGeneration || CapturePhase != ECapturePhase::Recording
		|| bFrozen || bCaptureFaulted)
	{
		return;
	}
	// Every event processed before this boundary was consumed by this engine frame.
	FCaptureOpenFrame Open;
	Open.CaptureFrameCounter = Boundary.CaptureFrameCounter;
	Open.FirstSequence = CaptureNextFirstSequence;
	Open.EventCount = CaptureEvents.Num() - CaptureNextFirstSequence;
	CaptureNextFirstSequence = CaptureEvents.Num();
	CaptureOpenFrame = Open;
}

bool FCortexReplayService::FImpl::AppendCaptureFrame(const FCortexEditorEngineFrameRecord& Observed)
{
	check(CaptureOpenFrame.IsSet());
	const FCaptureOpenFrame Open = CaptureOpenFrame.GetValue();
	CaptureOpenFrame.Reset();
	if (!(Observed.AppDeltaSeconds > 0.0 && Observed.AppDeltaSeconds <= CortexReplayMaxFrameDeltaSeconds))
	{
		MarkCaptureFaulted(ServiceError(CortexReplayErrorCodes::ReplayTimingError,
			FString::Printf(
				TEXT("An engine frame ran on a %g s application delta, outside the recordable (0, %g] s range"),
				Observed.AppDeltaSeconds, CortexReplayMaxFrameDeltaSeconds)));
		return false;
	}
	FCortexReplayFrame Frame;
	Frame.FrameIndex = CaptureFrames.Num();
	Frame.AppDeltaSeconds = Observed.AppDeltaSeconds;
	if (Observed.WorldTick.IsSet())
	{
		FCortexReplayFrameTick Tick;
		Tick.RealDeltaSeconds = Observed.WorldTick->RealDeltaSeconds;
		Tick.bPaused = Observed.WorldTick->bPaused;
		Frame.WorldTick = Tick;
	}
	Frame.FirstSequence = Open.FirstSequence;
	Frame.EventCount = Open.EventCount;
	CaptureFrames.Add(Frame);
	return true;
}

void FCortexReplayService::FImpl::OnCaptureFrameClosed(uint64 Generation,
	const FCortexEditorEngineFrameRecord& Closed)
{
	// A reentrant native callback only records evidence; it never publishes or tears down.
	if (Generation != CaptureOperationGeneration || CapturePhase != ECapturePhase::Recording
		|| bFrozen || bCaptureFaulted)
	{
		return;
	}
	if (Closed.SelectedWorldTickCount < 0 || !Closed.bBoundaryObserved)
	{
		MarkCaptureFaulted(ServiceError(CortexReplayErrorCodes::ReplayTimingError,
			TEXT("The engine frame observer invalidated a captured frame, so the recording would not match the frames the target consumed")));
		return;
	}
	if (!CaptureOpenFrame.IsSet() || CaptureOpenFrame->CaptureFrameCounter != Closed.CaptureFrameCounter)
	{
		return; // The epoch frame: its input boundary preceded the capture epoch.
	}
	AppendCaptureFrame(Closed);
}

void FCortexReplayService::FImpl::SealCaptureFrames()
{
	if (CaptureOpenFrame.IsSet())
	{
		// The frame whose boundary was observed but that has not closed: it is recorded when the
		// owned world ticked it, because that tick consumed the input attributed to it.
		const FCortexEditorEngineFrameRecord* Open =
			CaptureFrameObserver.IsValid() ? CaptureFrameObserver->GetOpenFrame() : nullptr;
		if (Open == nullptr || Open->CaptureFrameCounter != CaptureOpenFrame->CaptureFrameCounter)
		{
			CaptureOpenFrame.Reset();
			MarkCaptureFaulted(ServiceError(CortexReplayErrorCodes::ReplayTimingError,
				TEXT("The engine frame observer invalidated the final captured frame")));
		}
		else if (Open->WorldTick.IsSet())
		{
			AppendCaptureFrame(*Open);
		}
		else
		{
			CaptureOpenFrame.Reset();
		}
	}
	// Events after the last recorded boundary were not consumed by any recorded frame.
	const int32 AttributedEvents = CaptureFrames.Num() > 0
		? CaptureFrames.Last().FirstSequence + CaptureFrames.Last().EventCount : 0;
	CaptureEvents.SetNum(AttributedEvents);
	CaptureNextFirstSequence = AttributedEvents;
}

void FCortexReplayService::FImpl::DropTrailingStopChordPress()
{
	if (!FPlayWorldCommands::IsRegistered())
	{
		return;
	}
	const TSharedPtr<FUICommandInfo>& StopCommand = FPlayWorldCommands::Get().StopPlaySession;
	if (!StopCommand.IsValid())
	{
		return;
	}
	TSet<FKey> StopKeys;
	for (const EMultipleKeyBindingIndex ChordIndex :
		{ EMultipleKeyBindingIndex::Primary, EMultipleKeyBindingIndex::Secondary })
	{
		const FInputChord& Chord = *StopCommand->GetActiveChord(ChordIndex);
		if (Chord.IsValidChord())
		{
			StopKeys.Add(Chord.Key);
		}
	}
	const TSet<FKey> HeldStopKeys = CollectHeldCaptureKeys(CaptureEvents).Intersect(StopKeys);
	if (HeldStopKeys.IsEmpty())
	{
		return;
	}

	// The stop chord ends PIE in the same engine frame whose input pump delivered it (the deferred end
	// is processed by that frame's editor tick), so the ending gesture always lies in the final recorded
	// frame. A stop-key hold that began earlier was consumed as gameplay input; it is kept, and the stream
	// is then correctly withheld as unbalanced.
	if (CaptureFrames.IsEmpty())
	{
		return;
	}
	const int32 FinalFrameFirst = CaptureFrames.Last().FirstSequence;
	TArray<bool> Drop;
	Drop.Init(false, CaptureEvents.Num());
	for (const FKey& Key : HeldStopKeys)
	{
		// The key's current hold starts at its last non-repeat press that no release followed; that press
		// and every repeat after it belong to the hold.
		int32 HoldStart = INDEX_NONE;
		for (int32 Index = 0; Index < CaptureEvents.Num(); ++Index)
		{
			const FCortexEditorPhysicalInputEvent& Input = CaptureEvents[Index].Input;
			if (Input.Key != Key)
			{
				continue;
			}
			switch (Input.Kind)
			{
			case ECortexEditorPhysicalInputKind::KeyDown:
			case ECortexEditorPhysicalInputKind::PointerDown:
			case ECortexEditorPhysicalInputKind::DoubleClick:
				if (!Input.bRepeat || HoldStart == INDEX_NONE)
				{
					HoldStart = Index;
				}
				break;
			case ECortexEditorPhysicalInputKind::KeyUp:
			case ECortexEditorPhysicalInputKind::PointerUp:
				HoldStart = INDEX_NONE;
				break;
			default:
				break;
			}
		}
		if (HoldStart == INDEX_NONE || HoldStart < FinalFrameFirst)
		{
			continue;
		}
		for (int32 Index = HoldStart; Index < CaptureEvents.Num(); ++Index)
		{
			if (CaptureEvents[Index].Input.Key == Key)
			{
				Drop[Index] = true;
			}
		}
	}

	// Re-index the surviving events and re-attribute them to their frames.
	TArray<FCortexReplayEvent> Kept;
	Kept.Reserve(CaptureEvents.Num());
	for (FCortexReplayFrame& Frame : CaptureFrames)
	{
		const int32 NewFirst = Kept.Num();
		for (int32 Index = Frame.FirstSequence; Index < Frame.FirstSequence + Frame.EventCount; ++Index)
		{
			if (!Drop[Index])
			{
				FCortexReplayEvent Event = MoveTemp(CaptureEvents[Index]);
				Event.Sequence = Kept.Num();
				Kept.Add(MoveTemp(Event));
			}
		}
		Frame.FirstSequence = NewFirst;
		Frame.EventCount = Kept.Num() - NewFirst;
	}
	CaptureEvents = MoveTemp(Kept);
	CaptureNextFirstSequence = CaptureEvents.Num();
	UE_LOG(LogCortexReplay, Display,
		TEXT("Capture %d dropped the unreleased stop-play key press that ended the session"),
		CaptureRecordingId);
}

void FCortexReplayService::FImpl::OnCaptureEvent(const FCortexEditorPhysicalInputEvent& Event,
	double /*TimeSeconds*/, const FCortexEditorPhysicalInputCaptureContext& Context)
{
	if (CapturePhase != ECapturePhase::Recording || bFrozen || bCaptureFaulted)
	{
		return;
	}

	FCortexReplayEvent Recorded;
	Recorded.Sequence = CaptureEvents.Num();
	Recorded.Input = Event;

	const bool bPress = !Event.bRepeat
		&& (Event.Kind == ECortexEditorPhysicalInputKind::KeyDown
			|| Event.Kind == ECortexEditorPhysicalInputKind::PointerDown
			|| Event.Kind == ECortexEditorPhysicalInputKind::DoubleClick);
	if (bPress)
	{
		if (!Context.PressPose.IsSet())
		{
			// A press whose genuinely observed pre-press pose is absent must never be published
			// with a substituted guard; fault the recording instead.
			MarkCaptureFaulted(ServiceError(CortexReplayErrorCodes::InvalidOperation,
				TEXT("A captured press is missing its required pre-press player pose")));
			return;
		}
		FCortexReplayInteractionGuard Guard;
		Guard.ExpectedPose = Context.PressPose.GetValue();
		Guard.UICoverage = Context.UICoverage;
		Guard.UIUnavailableReason = Context.UIUnavailableReason;
		Guard.UITarget = Context.UITarget;
		Guard.ExpectedLocalPosition = Context.UILocalPosition;
		Recorded.Guard = Guard;
	}

	CaptureEvents.Add(MoveTemp(Recorded));
}

FCortexCommandResult FCortexReplayService::StartCapture(const FString& SavedEditorMapAssetPath)
{
	FImpl& State = *Impl;
	if (State.bShutdown)
	{
		return ServiceError(CortexReplayErrorCodes::InvalidOperation, TEXT("Replay service is shut down"));
	}
	if (State.bRunActive || State.bFinalizing || State.CapturePhase != FImpl::ECapturePhase::None)
	{
		return ServiceError(CortexErrorCodes::EditorBusy, TEXT("Another replay operation owns the target"));
	}

	// Admission precedes everything that has a side effect: a refused capture (unrelated PIE, missing
	// map, unsupported settings) consumes no recording id and installs no observer.
	const TSharedRef<FCortexEditorPhysicalInputSession> NewSession =
		MakeShared<FCortexEditorPhysicalInputSession>();
	const FCortexCommandResult Admission = NewSession->CheckOwnedPIEAdmission(SavedEditorMapAssetPath, 0);
	if (!Admission.bSuccess)
	{
		return Admission;
	}

	int32 ReservedId = 0;
	const FCortexCommandResult Reserved = State.Library.ReserveId(ReservedId);
	if (!Reserved.bSuccess)
	{
		return Reserved;
	}

	// Capture ownership is reserved for the whole owned preparation, not only after readiness.
	State.ResetCapture();
	// An admitted capture supersedes any retained terminal summary from a previous capture.
	State.ClearLastCaptureOutcome();
	State.CapturePhase = FImpl::ECapturePhase::Preparing;
	State.bOwnedCapture = true;
	State.CaptureRecordingId = ReservedId;
	++State.CaptureOperationGeneration;
	const uint64 Generation = State.CaptureOperationGeneration;
	UE_LOG(LogCortexReplay, Display, TEXT("Capture %d start accepted (owned, map '%s')"),
		State.CaptureRecordingId, *SavedEditorMapAssetPath);

	// A throttled editor can starve the game thread of input between frames; the override is
	// released by ResetCapture on every end path.
	State.CaptureThrottle.Acquire();

	State.Session = NewSession;
	FCortexReplayService::FImpl* RawState = &State;
	State.Session->SetInterruptionCallback(
		[RawState, Generation](const FCortexCommandResult& Interruption)
		{
			RawState->OnCaptureInterruption(Generation, Interruption);
		});

	// The observer is installed before the owned PIE request so the capture attributes every
	// input event to the engine frame that consumes it, at that frame's input boundary.
	State.CaptureFrameObserver = MakeUnique<FCortexEditorEngineFrameObserver>();
	const FCortexCommandResult ObserverInstalled = State.CaptureFrameObserver->Install(
		*State.Session,
		[RawState, Generation](const FCortexEditorEngineFrameRecord& Boundary)
		{
			RawState->OnCaptureInputBoundary(Generation, Boundary);
		},
		[RawState, Generation](const FCortexEditorEngineFrameRecord& Closed)
		{
			RawState->OnCaptureFrameClosed(Generation, Closed);
		});
	if (!ObserverInstalled.bSuccess)
	{
		State.MarkCaptureFaulted(ObserverInstalled);
		State.BeginCaptureFinalization(false);
		State.TickCapture();
		return ObserverInstalled;
	}

	const FCortexCommandResult Accepted = State.Session->BeginOwnedPIE(SavedEditorMapAssetPath, 0,
		[RawState, Generation](const FCortexCommandResult& Ready)
		{
			RawState->OnOwnedCaptureReady(Generation, Ready);
		});
	if (!Accepted.bSuccess)
	{
		State.MarkCaptureFaulted(Accepted);
		State.BeginCaptureFinalization(false);
		State.TickCapture();
		return Accepted;
	}

	UE_LOG(LogCortexReplay, Display, TEXT("Capture %d owned PIE request accepted for map '%s'"),
		State.CaptureRecordingId, *SavedEditorMapAssetPath);
	State.EnsureTicker();
	return ServiceSuccess();
}

FCortexCommandResult FCortexReplayService::StopCapture(bool bAbnormal)
{
	FImpl& State = *Impl;
	if (State.CapturePhase == FImpl::ECapturePhase::None)
	{
		return ServiceError(CortexReplayErrorCodes::InvalidOperation,
			TEXT("No capture is active"));
	}
	// A capture retained in Finalizing with a failed asynchronous publication reports that retained
	// failure; it is not "inactive" while the publication retry is still outstanding.
	if (State.CapturePhase == FImpl::ECapturePhase::Finalizing && State.bCapturePublicationFailed)
	{
		return State.CapturePublicationError;
	}
	// A faulted capture being discarded reports the fault that withheld its publication instead of
	// the generic inactive-operation rejection.
	if (State.CapturePhase == FImpl::ECapturePhase::Finalizing && State.bCaptureFaulted)
	{
		return State.CaptureFaultResult;
	}
	if (State.CapturePhase == FImpl::ECapturePhase::Finalizing)
	{
		return ServiceError(CortexReplayErrorCodes::InvalidOperation,
			TEXT("No capture is active"));
	}

	// A capture aborted before it began recording is never published. The open frame is sealed
	// first so a stream the observer invalidated is withheld rather than published.
	const bool bWasRecording = State.CapturePhase == FImpl::ECapturePhase::Recording;
	if (bWasRecording)
	{
		State.SealCaptureFrames();
	}
	const bool bPublish = !bAbnormal && !State.bCaptureFaulted && bWasRecording;

	// A non-publishing stop is reported distinctly: a fault carries its own reason, and a capture
	// that never reached Recording names the phase it was discarded from. An intentional abnormal
	// stop of a recording capture stays an accepted (successful) discard.
	FCortexCommandResult DiscardResult;
	if (State.bCaptureFaulted)
	{
		DiscardResult = State.CaptureFaultResult;
	}
	else if (!bWasRecording)
	{
		UE_LOG(LogCortexReplay, Display,
			TEXT("Capture %d discarded: never reached Recording (phase %s, faulted=%d)"),
			State.CaptureRecordingId, *FString(FImpl::CapturePhaseToString(State.CapturePhase)),
			State.bCaptureFaulted ? 1 : 0);
		DiscardResult = ServiceError(CortexReplayErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Capture %d was discarded: it never reached Recording (phase %s)"),
				State.CaptureRecordingId,
				*FString(FImpl::CapturePhaseToString(State.CapturePhase))));
	}

	UE_LOG(LogCortexReplay, Display,
		TEXT("Capture %d stop requested: phase=%s faulted=%d abnormal=%d publish=%d outcome=%s"),
		State.CaptureRecordingId, *FString(FImpl::CapturePhaseToString(State.CapturePhase)),
		State.bCaptureFaulted ? 1 : 0, bAbnormal ? 1 : 0, bPublish ? 1 : 0,
		bPublish ? TEXT("publish") : TEXT("discard"));

	// Owned capture retains its session and ownership until the matching teardown is observed.
	State.BeginCaptureFinalization(bPublish);
	State.TickCapture();
	// A publication attempt that already failed (and whose retries are outstanding) is reported
	// instead of a plain success; otherwise success means the stop was accepted and publication is
	// still pending, which the capture status exposes.
	if (State.bCapturePublicationFailed)
	{
		return State.CapturePublicationError;
	}
	if (!DiscardResult.ErrorCode.IsEmpty())
	{
		return DiscardResult;
	}
	return ServiceSuccess();
}
