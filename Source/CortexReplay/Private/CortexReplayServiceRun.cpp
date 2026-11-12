#include "CortexReplayServiceImpl.h"

#include "CortexReplayErrorCodes.h"
#include "CortexReplayInterruptionText.h"
#include "Widgets/SCortexReplayRunOverlay.h"

#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

using namespace CortexReplayServicePrivate;

namespace
{
/** The service's monotonic preparation deadline starts at native acceptance. */
constexpr double ServicePreparationDeadlineSeconds = 30.0;
/** A failed terminal write is retried at 1 Hz, like capture publication. */
constexpr double ServiceTerminalSaveRetrySeconds = 1.0;

/**
 * Reads only the live `ai_enabled` permission bit for a recording.
 *
 * The immutable admitted payload is never reloaded for this; a small metadata-only read keeps the
 * live permission check independent of the recorded inputs.
 */
bool ServiceReadMetadataAIEnabled(const FString& ProjectRoot, int32 RecordingId, bool& bOutEnabled)
{
	const FString Path = FPaths::Combine(
		FPaths::Combine(
			FPaths::Combine(ProjectRoot, TEXT(".cortex/replay/recordings")),
			FString::FromInt(RecordingId)),
		TEXT("metadata.json"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return false;
	}
	int32 ParsedId = 0;
	if (!Object->TryGetNumberField(TEXT("recording_id"), ParsedId) || ParsedId != RecordingId)
	{
		return false;
	}
	return Object->TryGetBoolField(TEXT("ai_enabled"), bOutEnabled);
}
}

FCortexCommandResult FCortexReplayService::FImpl::DispatchBlockError() const
{
	if (bInterruptionRequested && !InterruptionResult.ErrorCode.IsEmpty())
	{
		return InterruptionResult;
	}
	if (bCancellationRequested && !CancellationResult.ErrorCode.IsEmpty())
	{
		return CancellationResult;
	}
	return ServiceError(CortexReplayErrorCodes::InvalidOperation,
		TEXT("Replay dispatch is no longer active"));
}

bool FCortexReplayService::FImpl::CheckLivePermission()
{
	if (!bRunActive || bFinalizing || ActiveRun.Origin != ECortexReplayOrigin::AI)
	{
		return true;
	}
	if (bPermissionRevoked)
	{
		return false;
	}

	// The live grant is re-read once per engine frame: a cached value is never treated as fresh
	// authorization across frames, so an external revocation takes effect before the next frame's first
	// press, while a frame with many events costs one metadata read. Only the permission bit is read,
	// never the recorded inputs.
	if (PermissionCheckedFrame == GFrameCounter)
	{
		return true;
	}
	PermissionCheckedFrame = GFrameCounter;
	bool bEnabled = false;
	if (!ServiceReadMetadataAIEnabled(ProjectRoot, ActiveRun.RecordingId, bEnabled) || !bEnabled)
	{
		bPermissionRevoked = true;
		PermissionRevocationResult = ServiceError(CortexReplayErrorCodes::PermissionDenied,
			TEXT("AI replay permission was revoked"));
		bCancellationRequested = true;
		CancellationResult = PermissionRevocationResult;
		return false;
	}
	return true;
}

FCortexCommandResult FCortexReplayService::FImpl::CheckLivePrerequisites() const
{
	if (!Session.IsValid() || !Snapshot.IsValid())
	{
		return ServiceError(CortexReplayErrorCodes::TargetUnavailable,
			TEXT("Replay target session is unavailable"));
	}
	const FCortexEditorPhysicalInputTargetInfo& Live = Session->GetTargetInfo();
	const FCortexEditorPhysicalInputTargetInfo& Recorded = Snapshot->Metadata.Prerequisites;

	auto Mismatch = [](const TCHAR* Field, const FString& RecordedValue, const FString& LiveValue)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("field"), Field);
		Details->SetStringField(TEXT("recorded"), RecordedValue);
		Details->SetStringField(TEXT("live"), LiveValue);
		return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::IncompatiblePrerequisites),
			FString::Printf(TEXT("Replay prerequisite %s differs: recorded %s, live %s"),
				Field, *RecordedValue, *LiveValue),
			Details);
	};
	auto SizeToString = [](const FIntPoint& Size) { return FString::Printf(TEXT("%dx%d"), Size.X, Size.Y); };
	auto BoolToString = [](bool bValue) { return FString(bValue ? TEXT("true") : TEXT("false")); };

	if (Live.ViewportSize != Recorded.ViewportSize)
	{
		return Mismatch(TEXT("viewport_size"), SizeToString(Recorded.ViewportSize), SizeToString(Live.ViewportSize));
	}
	if (!FMath::IsNearlyEqual(Live.DpiScale, Recorded.DpiScale, 1e-6))
	{
		return Mismatch(TEXT("dpi_scale"), FString::SanitizeFloat(Recorded.DpiScale),
			FString::SanitizeFloat(Live.DpiScale));
	}
	if (Live.PlayerInputClass != Recorded.PlayerInputClass)
	{
		return Mismatch(TEXT("player_input_class"), Recorded.PlayerInputClass, Live.PlayerInputClass);
	}
	if (Live.InputConfigSha256 != Recorded.InputConfigSha256)
	{
		return Mismatch(TEXT("input_config_sha256"), Recorded.InputConfigSha256, Live.InputConfigSha256);
	}

	bool bMouseCapture = false;
	bool bHighPrecision = false;
	const FCortexCommandResult ViewportRead = Session->ReadViewportInputState(bMouseCapture, bHighPrecision);
	if (!ViewportRead.bSuccess)
	{
		return ViewportRead;
	}
	const FCortexReplayViewportState& RecordedViewport = Snapshot->InitialState.Viewport;
	if (bMouseCapture != RecordedViewport.bMouseCapture)
	{
		return Mismatch(TEXT("viewport_mouse_capture"), BoolToString(RecordedViewport.bMouseCapture),
			BoolToString(bMouseCapture));
	}
	if (bHighPrecision != RecordedViewport.bHighPrecision)
	{
		return Mismatch(TEXT("viewport_high_precision"), BoolToString(RecordedViewport.bHighPrecision),
			BoolToString(bHighPrecision));
	}
	return ServiceSuccess();
}

void FCortexReplayService::FImpl::InstallFrameHooks()
{
	if (BeginFrameHandle.IsValid())
	{
		return;
	}
	const TWeakPtr<FCortexReplayService::FImpl> Weak = AsShared();
	BeginFrameHandle = FCoreDelegates::OnBeginFrame.AddLambda([Weak]()
	{
		if (const TSharedPtr<FCortexReplayService::FImpl> Pinned = Weak.Pin())
		{
			Pinned->OnReplayBeginFrame();
		}
	});
	SamplingInputHandle = FCoreDelegates::OnSamplingInput.AddLambda([Weak]()
	{
		if (const TSharedPtr<FCortexReplayService::FImpl> Pinned = Weak.Pin())
		{
			Pinned->OnReplayInputBoundary();
		}
	});
	WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
		[Weak](UWorld* World, ELevelTick, float RealDeltaSeconds)
		{
			if (const TSharedPtr<FCortexReplayService::FImpl> Pinned = Weak.Pin())
			{
				Pinned->OnReplayWorldTickStart(World, RealDeltaSeconds);
			}
		});
}

void FCortexReplayService::FImpl::RemoveFrameHooks()
{
	FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
	FCoreDelegates::OnSamplingInput.Remove(SamplingInputHandle);
	FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
	BeginFrameHandle.Reset();
	SamplingInputHandle.Reset();
	WorldTickStartHandle.Reset();
	ArmedFrame = INDEX_NONE;
}

bool FCortexReplayService::FImpl::IsReplayingFrames() const
{
	return bRunActive && !bFinalizing && RunState == ECortexReplayState::Replaying
		&& Snapshot.IsValid() && Scheduler.IsValid() && !Scheduler->IsComplete()
		&& !bCancellationRequested && !bInterruptionRequested && !bBoundaryFailed;
}

bool FCortexReplayService::FImpl::IsLiveReplaying() const
{
	return bRunActive && !bFinalizing && RunState == ECortexReplayState::Replaying;
}

double FCortexReplayService::FImpl::GetEffectiveCountdownSeconds() const
{
	return Snapshot.IsValid() && !Snapshot->Metadata.bCountdownEnabled ? 0.0 : CountdownSeconds;
}

bool FCortexReplayService::FImpl::IsInCountdown() const
{
	return bRunActive && !bFinalizing && RunState == ECortexReplayState::Preparing
		&& GetEffectiveCountdownSeconds() > 0.0 && CountdownStartedSeconds > 0.0;
}

double FCortexReplayService::FImpl::GetCountdownRemainingSeconds() const
{
	if (!IsInCountdown())
	{
		return 0.0;
	}
	return FMath::Max(0.0, GetEffectiveCountdownSeconds() - (FPlatformTime::Seconds() - CountdownStartedSeconds));
}

void FCortexReplayService::FImpl::ShowRunOverlay()
{
	if (!Session.IsValid())
	{
		return;
	}
	UWorld* World = Session->GetTargetBinding().World.Get();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (Viewport == nullptr)
	{
		return;
	}
	if (RunOverlay.IsValid() && OverlayViewport.Get() == Viewport)
	{
		return;
	}
	// A different (or vanished) viewport than the one that holds the overlay: start over.
	RemoveRunOverlay();
	// The overlay is display only: hit-test invisible, so it never appears in the widget path under
	// the cursor, and never focusable, so showing it cannot change focus or the active window.
	RunOverlay = SNew(SCortexReplayRunOverlay).Visibility(EVisibility::HitTestInvisible);
	OverlayViewport = Viewport;
	Viewport->AddViewportWidgetContent(RunOverlay.ToSharedRef(), ReplayOverlayZOrder);
}

void FCortexReplayService::FImpl::UpdateCountdownOverlay()
{
	ShowRunOverlay();
	if (RunOverlay.IsValid())
	{
		RunOverlay->ShowCountdown(FMath::CeilToInt(GetCountdownRemainingSeconds()));
	}
}

void FCortexReplayService::FImpl::ShowPlayingOverlay()
{
	ShowRunOverlay();
	if (RunOverlay.IsValid() && Snapshot.IsValid())
	{
		RunOverlay->ShowPlaying(Snapshot->Metadata.Name);
	}
}

void FCortexReplayService::FImpl::RemoveRunOverlay()
{
	if (RunOverlay.IsValid())
	{
		if (UGameViewportClient* Viewport = OverlayViewport.Get())
		{
			Viewport->RemoveViewportWidgetContent(RunOverlay.ToSharedRef());
		}
	}
	RunOverlay.Reset();
	OverlayViewport.Reset();
}

FCortexCommandResult FCortexReplayService::FImpl::PrepareRecordedFrame(int32 FrameIndex,
	ECortexReplayFramePreparation& OutPreparation)
{
	return Scheduler->PrepareFrame(FrameIndex,
		[this]() { return FPlatformTime::Seconds() - ReplayEpoch; },
		[this](const FCortexReplayEvent& Event) { return EvaluateGuard(Event); },
		[this](const FCortexReplayEvent& Event) { return DispatchEvent(Event); },
		OutPreparation);
}

void FCortexReplayService::FImpl::OnReplayBeginFrame()
{
	if (!IsReplayingFrames())
	{
		// Nothing consumes recorded frames now, so the editor runs on its own clock again.
		ArmedFrame = INDEX_NONE;
		Clock.ReleaseFixedStep();
		return;
	}

	// The engine clock must still be exactly as this run left it. Another owner that switched fixed
	// stepping, replaced the step or installed a custom time step has taken application time away from
	// the recording; the run fails and that owner's clock is left untouched.
	FCortexCommandResult Boundary = Clock.VerifyOwnership();
	if (Boundary.bSuccess)
	{
		ArmedFrame = Scheduler->GetCompletedFrameCount();
		ArmedFrameCounter = GFrameCounter;
		bArmedFramePrepared = false;
		Clock.BeginFrameObservation();
		if (bCurrentFrameWaited)
		{
			// A readiness wait pauses the recorded timeline: its frames run on the editor's clock.
			Clock.ReleaseFixedStep();
			return;
		}
		Boundary = Clock.BeginPacedFrame(Snapshot->Frames[ArmedFrame].AppDeltaSeconds);
		if (Boundary.bSuccess)
		{
			ActiveRun.BehindSeconds = Clock.GetBehindSeconds();
			return;
		}
	}
	ArmedFrame = INDEX_NONE;
	bBoundaryFailed = true;
	BoundaryFailure = Boundary;
}

void FCortexReplayService::FImpl::OnReplayInputBoundary()
{
	if (!IsReplayingFrames() || ArmedFrame == INDEX_NONE || ArmedFrameCounter != GFrameCounter
		|| bArmedFramePrepared)
	{
		return;
	}
	// The live grant is read once at the top of every engine frame, so a revocation cancels the run on
	// its next frame even when the frame carries no events; TickRun finalizes from the flag it sets.
	if (!CheckLivePermission())
	{
		return;
	}
	ECortexReplayFramePreparation Preparation = ECortexReplayFramePreparation::Waiting;
	const FCortexCommandResult Prepared = PrepareRecordedFrame(ArmedFrame, Preparation);
	if (!Prepared.bSuccess)
	{
		bBoundaryFailed = true;
		BoundaryFailure = Prepared;
		return;
	}
	if (Preparation == ECortexReplayFramePreparation::Ready)
	{
		bArmedFramePrepared = true;
	}
	else
	{
		bCurrentFrameWaited = true;
	}
}

void FCortexReplayService::FImpl::OnReplayWorldTickStart(const UWorld* World, float RealDeltaSeconds)
{
	if (ArmedFrame == INDEX_NONE || ArmedFrameCounter != GFrameCounter || World == nullptr
		|| !Session.IsValid() || World != Session->GetTargetBinding().World.Get())
	{
		return;
	}
	Clock.ObserveWorldTick(RealDeltaSeconds, World->IsPaused());
}

void FCortexReplayService::FImpl::ResetRun()
{
	bRunActive = false;
	bFinalizing = false;
	bTerminalPublished = false;
	bFrozen = false;
	bOwnedPie = false;
	bReadySeen = false;
	bCancellationRequested = false;
	bInterruptionRequested = false;
	bPermissionRevoked = false;
	CancellationResult = FCortexCommandResult();
	InterruptionResult = FCortexCommandResult();
	PermissionRevocationResult = FCortexCommandResult();
	PreparationResult = FCortexCommandResult();
	PendingTerminalResult = FCortexCommandResult();
	PendingTerminalState = ECortexReplayState::Cancelled;
	ActiveRun = FCortexReplayRunRecord();
	Snapshot.Reset();
	Scheduler.Reset();
	PrepareDeadline = 0.0;
	ReplayEpoch = 0.0;
	CountdownStartedSeconds = 0.0;
	ArmedFrame = INDEX_NONE;
	ArmedFrameCounter = 0;
	bArmedFramePrepared = false;
	bCurrentFrameWaited = false;
	bBoundaryFailed = false;
	BoundaryFailure = FCortexCommandResult();
	PermissionCheckedFrame = TNumericLimits<uint64>::Max();
	bTerminalSaveFailed = false;
	LastTerminalSaveAttemptSeconds = 0.0;
	RemoveRunOverlay();
	// The clock is a synchronous save/restore pair, so every reset path can hand it back at once.
	RemoveFrameHooks();
	Clock.Release();
}

void FCortexReplayService::FImpl::TickRun()
{
	if (!bRunActive || bFinalizing || !Snapshot.IsValid())
	{
		return;
	}

	// The world (and its viewport) can vanish under a run; a stale overlay is only forgotten here,
	// the viewport released its content with it.
	if (RunOverlay.IsValid() && !OverlayViewport.IsValid())
	{
		RunOverlay.Reset();
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
			Owner->Finalize(ECortexReplayState::Error, ServiceError(
				CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
				TEXT("Replay preparation exceeded the monotonic deadline")));
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

		// The live grant and the target's compatibility are checked once, when readiness arrives; the
		// countdown that follows only watches that the target survives.
		if (CountdownStartedSeconds == 0.0)
		{
			// Recheck the live grant after readiness and before the epoch starts; only the permission
			// bit is read, never the admitted payload.
			if (!CheckLivePermission())
			{
				UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s lost eligibility after readiness: %s"),
					*ServiceGuidToString(ActiveRun.Id), *PermissionRevocationResult.ErrorMessage);
				Owner->Finalize(ECortexReplayState::Error, PermissionRevocationResult);
				return;
			}

			// The live target must match what the recording was captured on, before anything is replayed.
			const FCortexCommandResult Compatible = CheckLivePrerequisites();
			if (!Compatible.bSuccess)
			{
				UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s prerequisites mismatch: %s"),
					*ServiceGuidToString(ActiveRun.Id), *Compatible.ErrorMessage);
				Owner->Finalize(ECortexReplayState::Error, Compatible);
				return;
			}
			CountdownStartedSeconds = FPlatformTime::Seconds();
			// A target that became ready inside the preparation budget always gets its full countdown:
			// the countdown must not create a new preparation timeout.
			PrepareDeadline = FMath::Max(PrepareDeadline,
				CountdownStartedSeconds + GetEffectiveCountdownSeconds() + 1.0);
		}

		if (GetEffectiveCountdownSeconds() > 0.0)
		{
			FCortexCommandResult CountdownTargetError;
			if (!Session->ValidateTarget(CountdownTargetError))
			{
				Owner->Finalize(ECortexReplayState::Interrupted, CountdownTargetError);
				return;
			}
			if (IsInCountdown() && GetCountdownRemainingSeconds() > 0.0)
			{
				UpdateCountdownOverlay();
				return;
			}
			// The grant may have been revoked while the user was being warned.
			if (!CheckLivePermission())
			{
				UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s lost eligibility during the countdown: %s"),
					*ServiceGuidToString(ActiveRun.Id), *PermissionRevocationResult.ErrorMessage);
				Owner->Finalize(ECortexReplayState::Error, PermissionRevocationResult);
				return;
			}
		}

		// One-time pose restoration and immediate readback.
		const FCortexCommandResult Restored = Session->RestorePlayerPose(
			Snapshot->InitialState.Pose, Snapshot->InitialState.PawnClassPath);
		if (FPlatformTime::Seconds() >= PrepareDeadline)
		{
			Owner->Finalize(ECortexReplayState::Error, ServiceError(
				CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
				TEXT("Replay preparation exceeded the monotonic deadline")));
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

		// Final deadline check immediately before the epoch is established.
		if (FPlatformTime::Seconds() >= PrepareDeadline)
		{
			Owner->Finalize(ECortexReplayState::Error, ServiceError(
				CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
				TEXT("Replay preparation exceeded the monotonic deadline")));
			return;
		}

		Scheduler = MakeShared<FCortexReplayScheduler>(Snapshot.ToSharedRef());
		ReplayEpoch = FPlatformTime::Seconds();

		// Arm replay interference ownership at epoch establishment, before the first dispatch, so a
		// foreign focus/input established during the pre-first-event window is treated as
		// interference rather than being delivered to the foreign consumer. Real selected-route
		// input interrupts the epoch for every origin: the recording's origin never suppresses it.
		const FCortexCommandResult Epoch = Session->BeginReplayEpoch();
		if (!Epoch.bSuccess)
		{
			Owner->Finalize(ECortexReplayState::Interrupted, Epoch);
			return;
		}

		SetRunState(ECortexReplayState::Replaying);
		ShowPlayingOverlay();

		// The epoch frame R commits nothing: capture's epoch frame had already ticked when its epoch was
		// established, exactly like this engine frame. Recorded frame k is consumed by engine frame R+1+k.
	}

	if (RunState == ECortexReplayState::Replaying)
	{
		if (bCancellationRequested)
		{
			Owner->Finalize(ECortexReplayState::Cancelled,
				CancellationResult.bSuccess ? ServiceSuccess() : CancellationResult);
			return;
		}
		if (bInterruptionRequested)
		{
			Owner->Finalize(ECortexReplayState::Interrupted, InterruptionResult);
			return;
		}

		// The frame this engine frame armed was dispatched at its input boundary and consumed by the
		// world tick that just ran. It is committed only if that tick ran as the recording's did;
		// anything else means the recording no longer owns the clock. A frame that waited on UI
		// readiness ran on the editor's clock for its wait frames and is not verified.
		FCortexCommandResult Advanced = bBoundaryFailed ? BoundaryFailure : ServiceSuccess();
		if (!bBoundaryFailed && ArmedFrame != INDEX_NONE && ArmedFrameCounter == GFrameCounter
			&& bArmedFramePrepared)
		{
			if (!bCurrentFrameWaited)
			{
				const FCortexCommandResult Verified = Clock.VerifyFrame(Snapshot->Frames[ArmedFrame]);
				if (!Verified.bSuccess)
				{
					UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s lost its recorded time base: %s"),
						*ServiceGuidToString(ActiveRun.Id), *Verified.ErrorMessage);
					Owner->Finalize(ECortexReplayState::Error, Verified);
					return;
				}
			}
			Advanced = Scheduler->CommitFrame(ArmedFrame);
			ArmedFrame = INDEX_NONE;
			bCurrentFrameWaited = false;
		}

		ActiveRun.DispatchedEvents = Scheduler->GetDispatchedCount();
		ActiveRun.CompletedFrames = Scheduler->GetCompletedFrameCount();
		ActiveRun.AuthorizedWaitSeconds = Scheduler->GetAuthorizedWaitSeconds();

		if (!Advanced.bSuccess)
		{
			UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s dispatch failed: %s (%s)"),
				*ServiceGuidToString(ActiveRun.Id), *Advanced.ErrorCode, *Advanced.ErrorMessage);
			if (bInterruptionRequested)
			{
				Owner->Finalize(ECortexReplayState::Interrupted, InterruptionResult);
			}
			else if (bCancellationRequested)
			{
				Owner->Finalize(ECortexReplayState::Cancelled,
					CancellationResult.bSuccess ? ServiceSuccess() : CancellationResult);
			}
			else
			{
				Owner->Finalize(ECortexReplayState::Error, Advanced);
			}
			return;
		}

		// Completion is only claimed when no cancellation/interruption arrived at the boundary, and
		// a recording whose tail is idle still revalidates the live grant before completing.
		if (Scheduler->IsComplete())
		{
			CheckLivePermission();
			if (bCancellationRequested)
			{
				Owner->Finalize(ECortexReplayState::Cancelled,
					CancellationResult.bSuccess ? ServiceSuccess() : CancellationResult);
			}
			else if (bInterruptionRequested)
			{
				Owner->Finalize(ECortexReplayState::Interrupted, InterruptionResult);
			}
			else
			{
				Owner->Finalize(ECortexReplayState::Completed, ServiceSuccess());
			}
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
	if (Session.IsValid() && bOwnedPie && !bUnloading && !Session->IsOwnedPIEEnded())
	{
		return;
	}

	// The owned teardown is complete here, so the recording's clock is handed back before the terminal
	// result is published and before any successor operation is admitted.
	RemoveFrameHooks();
	Clock.Release();

	FCortexReplayRunRecord Terminal = ActiveRun;
	Terminal.State = PendingTerminalState;
	Terminal.FinalizedAtUtc = FDateTime::UtcNow();
	// The cause survives every end that has one; an Error always carries one.
	if (PendingTerminalState == ECortexReplayState::Completed)
	{
		Terminal.ExecutionError = FCortexCommandResult();
	}
	else
	{
		Terminal.ExecutionError = PendingTerminalResult;
		if (PendingTerminalState == ECortexReplayState::Interrupted && !PendingTerminalResult.ErrorCode.IsEmpty())
		{
			double StoppedAtSeconds = 0.0;
			if (Snapshot.IsValid())
			{
				const int32 FramesToSum = FMath::Min(ActiveRun.CompletedFrames, Snapshot->Frames.Num());
				for (int32 FrameIndex = 0; FrameIndex < FramesToSum; ++FrameIndex)
				{
					StoppedAtSeconds += Snapshot->Frames[FrameIndex].AppDeltaSeconds;
				}
			}
			Terminal.ExecutionError = CortexReplayInterruptionText::ToRunError(PendingTerminalResult,
				ActiveRun.CompletedFrames, ActiveRun.FrameCount, StoppedAtSeconds);
		}
		if (Terminal.ExecutionError.ErrorCode.IsEmpty() && PendingTerminalState == ECortexReplayState::Error)
		{
			// A terminal Error must always carry a bounded execution code so the retained record
			// stays valid and reloadable.
			Terminal.ExecutionError = ServiceError(CortexReplayErrorCodes::InvalidOperation,
				TEXT("Replay execution failed"));
		}
	}

	// Ownership is released only after the terminal record is durably indexed. A failed write is
	// retried once per second so a full disk neither floods the log nor the disk.
	const double SaveAttemptSeconds = FPlatformTime::Seconds();
	if (bTerminalSaveFailed && !bUnloading
		&& SaveAttemptSeconds - LastTerminalSaveAttemptSeconds < ServiceTerminalSaveRetrySeconds)
	{
		return;
	}
	const FCortexCommandResult Saved = RunStore.SaveTerminal(Terminal);
	if (!Saved.bSuccess)
	{
		bTerminalSaveFailed = true;
		LastTerminalSaveAttemptSeconds = SaveAttemptSeconds;
		UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s terminal persistence failed: %s (%s)"),
			*ServiceGuidToString(Terminal.Id), *Saved.ErrorCode, *Saved.ErrorMessage);
		if (!bUnloading)
		{
			return;
		}
	}
	bTerminalSaveFailed = false;

	ActiveRun = Terminal;
	bTerminalPublished = true;
	bFinalizing = false;
	bRunActive = false;
	SetRunState(Terminal.State);
	UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s finalized as %s"),
		*ServiceGuidToString(ActiveRun.Id), *FString(ServiceStateToString(ActiveRun.State)));

	ReleaseCaptureFrameObserver();
	FCortexEditorPhysicalInputSession::ShutdownAndRetainUntilOwnedTeardown(MoveTemp(Session));
	if (bShutdown)
	{
		DetachTickerAndSession();
	}

	// Last statement: a handler that reads the service sees the finished run. The record is copied so a
	// handler that touches the service cannot invalidate what the other handlers receive.
	if (!bShutdown)
	{
		const FCortexReplayRunRecord Published = Terminal;
		RunTerminalDelegate.Broadcast(Published);
	}
}

FCortexReplayGuardDecision FCortexReplayService::FImpl::EvaluateGuard(const FCortexReplayEvent& Event)
{
	FCortexReplayGuardDecision Decision;

	// A frozen or already-cancelled/interrupted run never evaluates further events.
	if (bFrozen || bCancellationRequested || bInterruptionRequested)
	{
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = DispatchBlockError();
		return Decision;
	}
	if (!CheckLivePermission())
	{
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = PermissionRevocationResult;
		return Decision;
	}

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
	// Interruption/cancellation arriving during evaluation must stop the same-time event drain.
	if (bFrozen || bCancellationRequested || bInterruptionRequested)
	{
		return DispatchBlockError();
	}
	if (!CheckLivePermission())
	{
		return PermissionRevocationResult;
	}
	if (!Session.IsValid())
	{
		return ServiceError(CortexReplayErrorCodes::TargetUnavailable, TEXT("Replay target is gone"));
	}
	return Session->Dispatch(Event.Input);
}

FCortexCommandResult FCortexReplayService::Finalize(ECortexReplayState TerminalState,
	const FCortexCommandResult& ExecutionResult)
{
	FImpl& State = *Impl;
	if (!State.bFinalizing)
	{
		State.bFinalizing = true;
		// The overlay leaves before anything of the owned session is torn down.
		State.RemoveRunOverlay();
		State.bFrozen = true;
		State.PendingTerminalState = TerminalState;
		State.PendingTerminalResult = ExecutionResult;
		// No recorded frame is consumed after finalization begins, so the editor gets its clock back
		// before the owned world's last frames run.
		State.ArmedFrame = INDEX_NONE;
		State.Clock.ReleaseFixedStep();

		if (State.Session.IsValid())
		{
			// Cleanup is judged against the original live ownership (world + controller), not mere
			// target readiness: a replaced pawn or unregistered viewport can still leave owned
			// input live on the original controller, while a never-bound or destroyed target has
			// nothing to neutralize.
			const FCortexEditorPhysicalInputTargetBinding& Binding = State.Session->GetTargetBinding();
			const bool bOriginalOwnershipLive = Binding.World.IsValid() && Binding.Controller.IsValid();

			const FCortexCommandResult Cleanup = State.Session->ReleaseHeldInputs();
			if (!Cleanup.bSuccess && bOriginalOwnershipLive
				&& State.PendingTerminalState != ECortexReplayState::Error)
			{
				State.PendingTerminalState = ECortexReplayState::Error;
				State.PendingTerminalResult = Cleanup;
			}
			else if (!Cleanup.bSuccess && State.PendingTerminalState == ECortexReplayState::Error
				&& State.PendingTerminalResult.ErrorCode.IsEmpty())
			{
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
	// The preparation deadline begins at native acceptance and covers snapshot validation.
	const double AcceptanceSeconds = FPlatformTime::Seconds();
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
	if (State.bRunActive || State.bFinalizing || State.CapturePhase != FImpl::ECapturePhase::None)
	{
		return Refuse(ServiceError(CortexErrorCodes::EditorBusy, TEXT("Another replay operation owns the target")));
	}

	TSharedPtr<const FCortexReplaySnapshot> SnapshotPtr;
	const FCortexCommandResult Loaded = State.Library.Load(Id, Origin == ECortexReplayOrigin::AI, SnapshotPtr);
	if (!Loaded.bSuccess)
	{
		return Refuse(Loaded);
	}
	if (FPlatformTime::Seconds() - AcceptanceSeconds >= ServicePreparationDeadlineSeconds)
	{
		return Refuse(ServiceError(CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
			TEXT("Replay preparation exceeded the monotonic deadline during snapshot validation")));
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
	Record.FrameCount = SnapshotPtr->Frames.Num();
	Record.FramesSha256 = SnapshotPtr->Metadata.FramesSha256;
	Record.GuardCoverage = SnapshotPtr->Metadata.GuardCoverage;

	State.ResetRun();
	State.ActiveRun = Record;
	State.Snapshot = SnapshotPtr;
	State.bRunActive = true;
	State.bOwnedPie = true;
	State.SetRunState(ECortexReplayState::Preparing);
	State.PrepareDeadline = AcceptanceSeconds + ServicePreparationDeadlineSeconds;

	// Admit the run to the engine clock before the owned PIE request: refuses an editor whose timing
	// another owner drives and disables the Slate throttle. Fixed stepping itself starts only with the
	// first recorded frame after the epoch, so the replayed world is born on the editor's clock.
	const FCortexCommandResult Admitted = State.Clock.Begin();
	if (!Admitted.bSuccess)
	{
		State.ResetRun();
		return Refuse(Admitted);
	}
	State.InstallFrameHooks();

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
	Data->SetNumberField(TEXT("countdown_seconds"), State.GetEffectiveCountdownSeconds());

	// Last statement of a successful admission, after all run state is set. Handlers must not start an
	// operation synchronously (same rule as the terminal delegate).
	const FCortexCommandResult Started = FCortexCommandRouter::Success(Data);
	State.RunAdmittedDelegate.Broadcast();
	return Started;
}

FCortexCommandResult FCortexReplayService::CancelReplay(const FGuid& Id, bool bAIOnly)
{
	FImpl& State = *Impl;
	const bool bMatchesActive = State.ActiveRun.Id == Id
		&& (!bAIOnly || State.ActiveRun.Origin == ECortexReplayOrigin::AI);
	if (bMatchesActive && State.bRunActive)
	{
		State.bCancellationRequested = true;
		State.CancellationResult = ServiceSuccess();
		Finalize(ECortexReplayState::Cancelled, ServiceSuccess());
		return FCortexCommandRouter::Success(State.BuildActiveRunData());
	}
	if (bMatchesActive && State.bTerminalPublished)
	{
		return FCortexCommandRouter::Success(BuildRunData(State.ActiveRun, false, nullptr));
	}

	FCortexReplayRunRecord Record;
	if (State.RunStore.Load(Id, ECortexReplayOrigin::AI, Record).bSuccess
		|| (!bAIOnly && State.RunStore.Load(Id, ECortexReplayOrigin::Human, Record).bSuccess))
	{
		// A pre-frame record has no frame identity, so cancel's full terminal access is unsupported
		// exactly as get_run's is; its compact recovery summary stays discoverable.
		if (Record.bLegacyFormat)
		{
			return ServiceError(CortexReplayErrorCodes::UnsupportedRunFormat,
				TEXT("Historical run records carry no frame identity"));
		}
		return FCortexCommandRouter::Success(BuildRunData(Record, false, nullptr));
	}
	return ServiceError(CortexReplayErrorCodes::RunNotFound,
		FString::Printf(TEXT("No replay run %s"), *ServiceGuidToString(Id)));
}
