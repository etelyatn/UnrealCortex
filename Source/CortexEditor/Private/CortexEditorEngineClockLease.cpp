#include "CortexEditorEngineClockLease.h"

#include "CoreGlobals.h"
#include "Containers/Ticker.h"
#include "CortexEditorModule.h"
#include "CortexEditorPhysicalInputSession.h"
#include "CortexEngineCompat.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace ClockLeaseCodes = CortexEditorClockLeaseErrorCodes;

namespace
{
	/** Native delta the engine's fixed branch actually stores (float widened back to double). */
	constexpr double CleanupDeltaAsStored =
		static_cast<double>(static_cast<float>(CortexEditorClockLease::CleanupDeltaSeconds));

	/**
	 * End-frame bound for locating the single normal fixed cleanup frame. The cleanup frame is
	 * the frame in which it is armed or the immediately following one, so more than a couple of
	 * engine frames without the native signature means the settings/profile no longer hold.
	 */
	constexpr int32 MaxCleanupWaitFrames = 4;

	/** Defensive cap on the bounded cleanup wall alignment; the real wait is at most one delta. */
	constexpr double MaxCleanupWallWaitSeconds = 1.0;
}

/**
 * Owner-side lease state.
 *
 * The state outlives every in-flight clock callback because the owner holds it with a
 * TSharedPtr while the transient clock only keeps a weak link. Delegates are therefore
 * always reached through a pinned shared pointer, so removing observers while the engine
 * is broadcasting, a successor replacing the provider and a destroyed owner can never
 * touch freed memory or drop an in-flight root.
 */
struct FCortexEditorEngineClockLeaseState;
void RestoreOwnedSettingsOnStateDestroyed(FCortexEditorEngineClockLeaseState& State);

struct FCortexEditorEngineClockLeaseState
{
	/**
	 * Last-resort safety: a state destroyed while a cleanup frame is armed must not leave this
	 * lease's engine-global fixed-step edits installed. Defined out-of-line next to
	 * RestoreOwnedSettings, which only rewrites values still exactly ours.
	 */
	~FCortexEditorEngineClockLeaseState()
	{
		RestoreOwnedSettingsOnStateDestroyed(*this);
	}

	/** Weak self so delegate lambdas never extend the lease lifetime. */
	TWeakPtr<FCortexEditorEngineClockLeaseState> WeakSelf;

	/** The engine this lease is attached to; never a strong UObject reference. */
	TWeakObjectPtr<UEngine> Engine;

	/** Strongly rooted transient provider. */
	TStrongObjectPtr<UCortexEditorRecordedFrameClock> Clock;

	uint64 OperationGeneration = 0;
	ECortexEditorClockState State = ECortexEditorClockState::Released;

	/** Stored once; never copied per frame. */
	TFunction<FCortexCommandResult(FCortexEditorAppClockStep&)> ReadNextStep;

	// Acquisition snapshot; only values that are still exactly ours are ever restored.
	bool bOriginalUseFixedFrameRate = false;
	bool bOriginalUseFixedTimeStep = false;
	double OriginalFixedDeltaTime = 1.0 / 30.0;
	bool bOriginalBenchmarking = false;
	bool bOriginalOverrideFps = false;

	// Cleanup: one normal fixed engine frame plus the following default observation. Used by the
	// normal handoff and by the provider-cleared reanchor; the observed result state records
	// which truth that cleanup must end in.
	bool bCleanupStarted = false;
	bool bCleanupFrameHandled = false;
	int32 CleanupWaitFrames = 0;
	double CleanupBaseCurrent = 0.0;
	double CleanupToCurrent = 0.0;
	ECortexEditorClockState CleanupResultState = ECortexEditorClockState::Released;
	bool bOwnsFixedTimeStepEdit = false;
	bool bOwnsFixedDeltaEdit = false;

	FDelegateHandle CleanupEndFrameHandle;
	bool bCleanupEndFrameRegistered = false;

	FDelegateHandle CustomTimeStepChangedHandle;
	bool bCustomTimeStepChangedRegistered = false;

	/** True only while this lease itself clears the engine provider. */
	bool bOwnDetachInProgress = false;

	/**
	 * Depth of engine callbacks currently executing ON the owned clock. Retirement while this is
	 * nonzero must not detach or release the object that is still executing, so the real release is
	 * deferred to a later tick once the engine's clock call stack has fully unwound.
	 */
	int32 ExecutionDepth = 0;
	bool bDeferredReleasePending = false;
	bool bDeferredRestoreSettings = false;
	FTSTicker::FDelegateHandle DeferredReleaseHandle;

	/** Latched failure (readiness/pacing/timing); never cleared by success paths. */
	bool bHasFailure = false;
	FCortexCommandResult LastFailure;

#if WITH_DEV_AUTOMATION_TESTS
	/** Consumed once by Initialize to reproduce the initialized-false/non-null hazard. */
	bool bForceInitializeFailureForTests = false;
#endif
};

namespace
{
	using FClockState = FCortexEditorEngineClockLeaseState;

	FCortexCommandResult MakeClockLeaseError(const FString& Code, const FString& Message)
	{
		FCortexCommandResult Result;
		Result.bSuccess = false;
		Result.ErrorCode = Code;
		Result.ErrorMessage = Message;
		return Result;
	}

	FCortexCommandResult MakeClockLeaseError(const FString& Code, const FString& Message, const FString& Reason)
	{
		FCortexCommandResult Result = MakeClockLeaseError(Code, Message);
		Result.AddContext(TEXT("reason"), Reason);
		return Result;
	}

	FCortexCommandResult MakeSuccess()
	{
		FCortexCommandResult Result;
		Result.bSuccess = true;
		return Result;
	}

	/** Cached once: the console variable outlives every game-thread callback. */
	const IConsoleVariable* GetOverrideFpsCVar()
	{
#if !UE_BUILD_SHIPPING
		static const IConsoleVariable* const Cached = IConsoleManager::Get().FindConsoleVariable(TEXT("t.OverrideFPS"));
		return Cached;
#else
		return nullptr;
#endif
	}

	/** Explicit application-delta override (t.OverrideFPS); a normal t.MaxFPS cap is allowed. */
	bool IsOverrideFpsActive()
	{
		const IConsoleVariable* const OverrideCVar = GetOverrideFpsCVar();
		return OverrideCVar != nullptr && OverrideCVar->GetFloat() >= 0.001f;
	}

	/** True while a PIE world context exists or a play session request is still queued. */
	bool HasActivePIEWorld()
	{
		if (GEditor == nullptr)
		{
			return true;
		}
		if (GEditor->PlayWorld != nullptr)
		{
			return true;
		}
		if (GEditor->GetPlaySessionRequest().IsSet())
		{
			return true;
		}
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE)
				{
					return true;
				}
			}
		}
		return false;
	}

	/**
	 * Admission profile check: everything the engine would consume or tear down before our own
	 * request must be absent, and the measured normal clock must be reconstructible.
	 */
	bool HasUnrelatedPIESession()
	{
		if (GEditor == nullptr)
		{
			return true;
		}
		if (GEditor->ShouldEndPlayMap())
		{
			return true;
		}
		return HasActivePIEWorld();
	}

	// -- forward declarations -------------------------------------------------------------
	void HandleCleanupEndFrame(FClockState& State);
	void HandleProviderChanged(FClockState& State);
	void RestoreOwnedSettings(FClockState& State);
	void RemoveObservers(FClockState& State);
	void ReleaseOwnedResources(FClockState& State, bool bRestoreSettings);
	void ScheduleDeferredRelease(FClockState& State);
	void FinishLostCleanup(FClockState& State, const FString& Reason);
	void ArmCleanupFrame(FClockState& State, ECortexEditorClockState ResultState);
	void LatchLost(FClockState& State, const FString& Code, const FString& Reason);
	bool VerifyProfileUnchanged(const FClockState& State, FString& OutReason);
	bool FailStep(FClockState& State, const FString& Code, const FString& Reason);
	bool UpdateTimeStepImpl(FClockState& State, UEngine& Engine, UCortexEditorRecordedFrameClock& Clock);

	// -- helpers --------------------------------------------------------------------------

	/**
	 * Restores ONLY values that are still exactly the ones this lease wrote. A foreign owner
	 * that changed a setting is never overwritten, so a successor is never clobbered.
	 */
	void RestoreOwnedSettings(FClockState& State)
	{
		if (State.bOwnsFixedTimeStepEdit)
		{
			if (FApp::UseFixedTimeStep())
			{
				FApp::SetUseFixedTimeStep(State.bOriginalUseFixedTimeStep);
			}
			State.bOwnsFixedTimeStepEdit = false;
		}
		if (State.bOwnsFixedDeltaEdit)
		{
			if (FApp::GetFixedDeltaTime() == CortexEditorClockLease::CleanupDeltaSeconds)
			{
				FApp::SetFixedDeltaTime(State.OriginalFixedDeltaTime);
			}
			State.bOwnsFixedDeltaEdit = false;
		}
	}

	void RemoveObservers(FClockState& State)
	{
		if (UEngine* Engine = State.Engine.Get())
		{
			if (State.bCustomTimeStepChangedRegistered)
			{
				Engine->OnCustomTimeStepChanged().Remove(State.CustomTimeStepChangedHandle);
			}
		}
		State.bCustomTimeStepChangedRegistered = false;
		State.CustomTimeStepChangedHandle.Reset();

		if (State.bCleanupEndFrameRegistered)
		{
			FCoreDelegates::OnEndFrame.Remove(State.CleanupEndFrameHandle);
		}
		State.bCleanupEndFrameRegistered = false;
		State.CleanupEndFrameHandle.Reset();
	}

	void ReleaseOwnedResources(FClockState& State, bool bRestoreSettings)
	{
		RemoveObservers(State);

		if (UEngine* Engine = State.Engine.Get())
		{
			// Clear the engine provider only while it is still this exact clock; a successor
			// installed during our Shutdown is left untouched. The detach stays immediate even when
			// we are executing: the engine must stop calling a retired clock.
			if (State.Clock.IsValid() && Engine->GetCustomTimeStep() == State.Clock.Get())
			{
				State.bOwnDetachInProgress = true;
				Engine->SetCustomTimeStep(nullptr);
				State.bOwnDetachInProgress = false;
			}
		}

		if (bRestoreSettings)
		{
			RestoreOwnedSettings(State);
		}

		// Retirement stops the exact owned consumer, so a retired wrapper admits no further owned
		// world tick. Idempotent and safe when nothing is retained.
		FCortexEditorPhysicalInputSession::QuiesceRetainedOwnedAuthority();

		if (State.ExecutionDepth > 0)
		{
			// The clock object is still executing on the engine's stack (the lease owner was
			// destroyed from inside its own callback). Keep it alive until that stack unwinds, then
			// release it on a later tick.
			State.bDeferredReleasePending = true;
			ScheduleDeferredRelease(State);
			return;
		}

		State.Clock.Reset();
	}

	/** One-shot next-tick release for a retirement that arrived while the clock was executing. */
	void ScheduleDeferredRelease(FClockState& State)
	{
		if (State.DeferredReleaseHandle.IsValid())
		{
			return;
		}
		const TWeakPtr<FClockState> Weak = State.WeakSelf;
		State.DeferredReleaseHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Weak](float) -> bool
			{
				const TSharedPtr<FClockState> Pinned = Weak.Pin();
				if (!Pinned.IsValid())
				{
					return false;
				}
				Pinned->DeferredReleaseHandle.Reset();
				if (Pinned->bDeferredReleasePending && Pinned->ExecutionDepth == 0)
				{
					const bool bRestore = Pinned->bDeferredRestoreSettings;
					Pinned->bDeferredReleasePending = false;
					Pinned->bDeferredRestoreSettings = false;
					ReleaseOwnedResources(*Pinned, bRestore);
				}
				return false; // One-shot.
			}));
	}

	/**
	 * Waits (bounded by the cleanup delta) until platform wall time reaches the application
	 * current produced by the cleanup frame, so the following default frame cannot move
	 * application time backward. Mirrors the engine's own sleep-then-yield wait pattern.
	 * Returns false only if the wait exceeded its defensive cap, which would mean the target is
	 * no longer the one cleanup delta past the current wall time.
	 */
	bool AlignWallToCleanupTo(const FClockState& State)
	{
		const double Target = State.CleanupToCurrent;
		double Wait = Target - FPlatformTime::Seconds();
		if (Wait <= 0.0)
		{
			return true;
		}
		if (Wait > MaxCleanupWallWaitSeconds)
		{
			return false;
		}
		if (Wait > 5.0 / 1000.0)
		{
			FPlatformProcess::SleepNoStats(static_cast<float>(Wait - 0.002));
		}
		while (FPlatformTime::Seconds() < Target)
		{
			FPlatformProcess::SleepNoStats(0.0f);
		}
		return true;
	}

	void RegisterCleanupObserver(FClockState& State)
	{
		if (State.bCleanupEndFrameRegistered)
		{
			return;
		}
		const TWeakPtr<FClockState> Weak = State.WeakSelf;
		State.CleanupEndFrameHandle = FCoreDelegates::OnEndFrame.AddLambda([Weak]()
		{
			if (const TSharedPtr<FClockState> Pinned = Weak.Pin())
			{
				HandleCleanupEndFrame(*Pinned);
			}
		});
		State.bCleanupEndFrameRegistered = true;
	}

	/**
	 * Takes exactly the fixed-step settings needed for ONE engine frame and arms the two
	 * end-frame observations. This is the same public one-frame transition the handoff uses,
	 * so a provider that was cleared externally is reanchored without private engine state.
	 */
	void ArmCleanupFrame(FClockState& State, ECortexEditorClockState ResultState)
	{
		State.CleanupBaseCurrent = FApp::GetCurrentTime();
		State.CleanupResultState = ResultState;
		State.bCleanupStarted = true;
		State.bCleanupFrameHandled = false;
		State.CleanupWaitFrames = 0;

		State.bOwnsFixedTimeStepEdit = true;
		State.bOwnsFixedDeltaEdit = true;
		FApp::SetUseFixedTimeStep(true);
		FApp::SetFixedDeltaTime(CortexEditorClockLease::CleanupDeltaSeconds);

		RegisterCleanupObserver(State);
	}

	void FinishLostCleanup(FClockState& State, const FString& Reason)
	{
		if (State.State == ECortexEditorClockState::Released)
		{
			return;
		}
		State.State = ECortexEditorClockState::Lost;
		if (State.bHasFailure)
		{
			State.LastFailure.AddContext(TEXT("cleanup"), Reason);
		}
		else
		{
			State.bHasFailure = true;
			State.LastFailure = MakeClockLeaseError(ClockLeaseCodes::HandoffNotObserved,
				TEXT("The recorded-frame clock cleanup did not complete truthfully"), Reason);
		}
		UE_LOG(LogCortexEditor, Display, TEXT("Recorded-frame clock cleanup failed: %s"), *Reason);
		ReleaseOwnedResources(State, /*bRestoreSettings*/ true);
	}

	void LatchLost(FClockState& State, const FString& Code, const FString& Reason)
	{
		if (State.State == ECortexEditorClockState::Released || State.State == ECortexEditorClockState::Lost)
		{
			return;
		}
		State.State = ECortexEditorClockState::Lost;
		if (!State.bHasFailure)
		{
			State.bHasFailure = true;
			State.LastFailure = MakeClockLeaseError(Code, TEXT("The exclusive recorded-frame clock lease was lost"), Reason);
		}
		UE_LOG(LogCortexEditor, Display, TEXT("Recorded-frame clock lease lost (%s): %s"), *Code, *Reason);

		// Terminal ownership cleanup: the exact owned consumer must stop before the engine returns to
		// world processing, even if the caller backend already retired. Quiesce (never end) the
		// retained exact-session authority so no further owned world tick is admitted and a
		// world-less birth is stopped before its first tick. Idempotent, preserves this failure and
		// the held-provider clock state, and never touches a foreign instance.
		FCortexEditorPhysicalInputSession::QuiesceRetainedOwnedAuthority();

		// If an external actor cleared the provider, nobody owns engine time and the default
		// real-time cache is stale. Use the engine's own public one-frame fixed transition to
		// refresh it, so the following default frame must be bounded and forward. A successor
		// provider or any foreign timing edit is never touched.
		//
		// The reanchor is armed ONLY once no PIE world remains: the cleanup frame is consumed by
		// whichever world is live, and this engine-global lease cannot prove matching-world
		// ownership from its operation generation alone. Arming while a PIE world is alive would
		// fabricate a fixed-step frame for a world the lease may not own.
		FString ProfileReason;
		const UEngine* const Engine = State.Engine.Get();

		// Cleanup-only reanchor requires exact terminal-owned authority, unchanged timing settings and
		// NO foreign topology: any live PIE world must be exactly the owned, already-quiesced one. The
		// retained scoped authority proves that; without it, fall back to the conservative rule that
		// no PIE world at all may remain. The arm is safe because the state's destructor restores any
		// engine-global fixed-step edit this lease installed, so a teardown mid-cleanup cannot leak it.
		bool bNoForeignPlayWork = !HasActivePIEWorld();
		{
			FCortexScopedPIESnapshot Retained;
			const FCortexCommandResult Read = FCortexEditorPhysicalInputSession::ReadRetainedOwnedAuthority(Retained);
			if (Read.bSuccess)
			{
				bNoForeignPlayWork = !Retained.bHasForeignPlayWork;
			}
		}

		if (!State.bCleanupStarted
			&& Engine != nullptr
			&& State.Clock.IsValid()
			&& Engine->GetCustomTimeStep() == nullptr
			&& bNoForeignPlayWork
			&& VerifyProfileUnchanged(State, ProfileReason))
		{
			ArmCleanupFrame(State, ECortexEditorClockState::Lost);
			return;
		}
		ReleaseOwnedResources(State, /*bRestoreSettings*/ true);
	}

	/** Verifies no foreign edit invalidated the acquisition snapshot or our owned edits. */
	bool VerifyProfileUnchanged(const FClockState& State, FString& OutReason)
	{
		UEngine* Engine = State.Engine.Get();
		if (Engine == nullptr)
		{
			OutReason = TEXT("engine_unavailable");
			return false;
		}
		if (FApp::IsBenchmarking() != State.bOriginalBenchmarking)
		{
			OutReason = TEXT("benchmarking_changed");
			return false;
		}
		if (!!Engine->bUseFixedFrameRate != State.bOriginalUseFixedFrameRate)
		{
			OutReason = TEXT("fixed_frame_rate_changed");
			return false;
		}
		if (IsOverrideFpsActive() != State.bOriginalOverrideFps)
		{
			OutReason = TEXT("delta_override_changed");
			return false;
		}

		if (State.bOwnsFixedTimeStepEdit)
		{
			if (!FApp::UseFixedTimeStep())
			{
				OutReason = TEXT("owned_fixed_time_step_clobbered");
				return false;
			}
		}
		else if (FApp::UseFixedTimeStep() != State.bOriginalUseFixedTimeStep)
		{
			OutReason = TEXT("fixed_time_step_changed");
			return false;
		}

		if (State.bOwnsFixedDeltaEdit
			&& FApp::GetFixedDeltaTime() != CortexEditorClockLease::CleanupDeltaSeconds)
		{
			OutReason = TEXT("owned_fixed_delta_clobbered");
			return false;
		}
		return true;
	}

	/**
	 * Latches a rejected step. The lease changes no application time: it stops advancing and
	 * returns false so the engine never falls through to its own normal/zero/default clock.
	 */
	bool FailStep(FClockState& State, const FString& Code, const FString& Reason)
	{
		State.bHasFailure = true;
		State.LastFailure = MakeClockLeaseError(Code, TEXT("The recorded application clock step was rejected"), Reason);
		UE_LOG(LogCortexEditor, Display, TEXT("Recorded-frame clock step rejected (%s): %s"), *Code, *Reason);
		return false;
	}

	void RegisterProviderObserver(FClockState& State)
	{
		UEngine* Engine = State.Engine.Get();
		if (Engine == nullptr || State.bCustomTimeStepChangedRegistered)
		{
			return;
		}
		const TWeakPtr<FClockState> Weak = State.WeakSelf;
		State.CustomTimeStepChangedHandle = Engine->OnCustomTimeStepChanged().AddLambda([Weak]()
		{
			if (const TSharedPtr<FClockState> Pinned = Weak.Pin())
			{
				HandleProviderChanged(*Pinned);
			}
		});
		State.bCustomTimeStepChangedRegistered = true;
	}

	void HandleProviderChanged(FClockState& State)
	{
		if (State.State == ECortexEditorClockState::Released || State.State == ECortexEditorClockState::Lost)
		{
			return;
		}
		if (State.bOwnDetachInProgress)
		{
			// Our own handoff cleared the provider; this is not a replacement.
			return;
		}
		UEngine* Engine = State.Engine.Get();
		if (Engine == nullptr || !State.Clock.IsValid())
		{
			return;
		}
		if (Engine->GetCustomTimeStep() != State.Clock.Get())
		{
			LatchLost(State, ClockLeaseCodes::AttachmentLost,
				TEXT("a foreign custom time step replaced the owned recorded-frame clock"));
		}
	}

	/**
	 * Observes the single normal fixed cleanup frame by its exact native signature
	 * (LastTime == base, CurrentTime == base + stored delta, DeltaTime == stored delta), then
	 * observes the following default real-time frame.
	 */
	void HandleCleanupEndFrame(FClockState& State)
	{
		if (!State.bCleanupStarted)
		{
			return;
		}

		if (!State.bCleanupFrameHandled)
		{
			// Owned settings must still be exactly ours; a foreign owner that changed them is
			// never overwritten and never gets our restore.
			if (!FApp::UseFixedTimeStep() || FApp::GetFixedDeltaTime() != CortexEditorClockLease::CleanupDeltaSeconds)
			{
				FinishLostCleanup(State, TEXT("owned fixed-step settings changed during the cleanup frame"));
				return;
			}

			const double Base = State.CleanupBaseCurrent;
			if (FApp::GetLastTime() == Base
				&& FApp::GetCurrentTime() == Base + CleanupDeltaAsStored
				&& FApp::GetDeltaTime() == CleanupDeltaAsStored)
			{
				// End of the single normal fixed cleanup frame: the engine's cached real time now
				// equals the logical current, so the default frame can reanchor from it.
				State.CleanupToCurrent = FApp::GetCurrentTime();
				if (!AlignWallToCleanupTo(State))
				{
					FinishLostCleanup(State, TEXT("cleanup wall alignment exceeded its bounded wait"));
					return;
				}
				RestoreOwnedSettings(State);
				State.bCleanupFrameHandled = true;
				State.CleanupWaitFrames = 0;
				return;
			}
			if (++State.CleanupWaitFrames > MaxCleanupWaitFrames)
			{
				FinishLostCleanup(State, TEXT("the single normal fixed cleanup frame was not observed"));
			}
			return;
		}

		// The following engine frame is the default real-time frame.
		UEngine* const Engine = State.Engine.Get();
		if (Engine != nullptr && Engine->GetCustomTimeStep() != nullptr)
		{
			FinishLostCleanup(State, TEXT("a foreign custom time step owns engine time after the cleanup frame"));
			return;
		}

		FString ProfileReason;
		if (!VerifyProfileUnchanged(State, ProfileReason))
		{
			FinishLostCleanup(State, FString::Printf(
				TEXT("timing profile changed before the default frame: %s"), *ProfileReason));
			return;
		}

		const double Current = FApp::GetCurrentTime();
		const double Last = FApp::GetLastTime();
		const double Delta = FApp::GetDeltaTime();
		const bool bResumeOk = FMath::IsFinite(Current) && FMath::IsFinite(Last) && FMath::IsFinite(Delta)
			&& Delta > 0.0
			&& Delta <= CortexEditorClockLease::DefaultFrameMaxDeltaSeconds
			&& Current >= State.CleanupToCurrent
			&& Current >= Last;

		if (!bResumeOk)
		{
			FinishLostCleanup(State, FString::Printf(
				TEXT("default clock frame not positive/bounded/non-backward: current=%.9f last=%.9f delta=%.9f cleanup=%.9f"),
				Current, Last, Delta, State.CleanupToCurrent));
			return;
		}

		State.State = State.CleanupResultState; // Released for a handoff, Lost for a reanchor
		ReleaseOwnedResources(State, /*bRestoreSettings*/ false);
		if (State.State == ECortexEditorClockState::Released)
		{
			UE_LOG(LogCortexEditor, Log, TEXT("Recorded-frame clock lease released with an observed default frame"));
		}
	}

	/**
	 * The one normal fixed cleanup frame for a requested handoff. Called from the engine's
	 * update-time callback, so returning true makes THIS SAME engine update run the engine's
	 * normal fixed branch; no extra held-delta frame is produced.
	 */
	bool RunCleanupFrame(FClockState& State, UEngine& Engine, UCortexEditorRecordedFrameClock& Clock)
	{
		UEngineCustomTimeStep* const Provider = Engine.GetCustomTimeStep();
		if (Provider != nullptr && Provider != &Clock)
		{
			LatchLost(State, ClockLeaseCodes::AttachmentLost,
				TEXT("a foreign custom time step replaced the owned clock before handoff"));
			return false;
		}

		// The cleanup frame is consumed by whichever world is live. A PIE world that appeared
		// after BeginHandoff cannot be proven to be the matching owned world, so the cleanup
		// frame is never armed for it.
		if (HasActivePIEWorld())
		{
			LatchLost(State, CortexErrorCodes::EditorBusy,
				TEXT("a PIE world is live at the clock handoff frame"));
			return false;
		}

		FString Reason;
		if (!VerifyProfileUnchanged(State, Reason))
		{
			LatchLost(State, ClockLeaseCodes::SettingsChanged,
				FString::Printf(TEXT("foreign timing-profile edit before clock handoff: %s"), *Reason));
			return false;
		}

		ArmCleanupFrame(State, ECortexEditorClockState::Released);

		// Detach only this actual provider; a null setter returning false is not an error.
		State.bOwnDetachInProgress = true;
		if (Engine.GetCustomTimeStep() == &Clock)
		{
			Engine.SetCustomTimeStep(nullptr);
		}
		State.bOwnDetachInProgress = false;

		// Read the actual provider back: a successor installed during Shutdown owns time now and
		// must never have the engine's default branch run over it.
		if (Engine.GetCustomTimeStep() != nullptr)
		{
			LatchLost(State, ClockLeaseCodes::AttachmentLost,
				TEXT("a successor replaced the clock during the handoff detach"));
			return false;
		}
		return true;
	}

	bool RunTimedFrame(FClockState& State, UEngine& Engine, UCortexEditorRecordedFrameClock& Clock)
	{
		if (State.bHasFailure)
		{
			// Preserve the failed clock without inventing an application instant. The engine
			// still advances application GameTime by the previous delta; only the caller's
			// exact owned-session terminal tick-admission gate can prevent a world consumer.
			return false;
		}

		FString ProfileReason;
		if (!VerifyProfileUnchanged(State, ProfileReason))
		{
			LatchLost(State, ClockLeaseCodes::SettingsChanged,
				FString::Printf(TEXT("foreign timing-profile edit during playback: %s"), *ProfileReason));
			return Engine.GetCustomTimeStep() == nullptr;
		}

		if (!State.ReadNextStep)
		{
			LatchLost(State, CortexErrorCodes::InvalidOperation, TEXT("read_callback_missing"));
			return Engine.GetCustomTimeStep() == nullptr;
		}

		FCortexEditorAppClockStep Step;
		const FCortexCommandResult Read = State.ReadNextStep(Step);

		// A reentrant callback may have changed the lease state; never apply a step under the
		// wrong state.
		if (State.State != ECortexEditorClockState::Preparing && State.State != ECortexEditorClockState::Replaying)
		{
			return UpdateTimeStepImpl(State, Engine, Clock);
		}
		// A reentrant callback may also have destroyed the owner or replaced the provider.
		if (Engine.GetCustomTimeStep() != &Clock)
		{
			LatchLost(State, ClockLeaseCodes::AttachmentLost,
				TEXT("the lease was released or replaced during the step callback"));
			return Engine.GetCustomTimeStep() == nullptr;
		}

		if (!Read.bSuccess)
		{
			// Preserve the producer's failure without manufacturing application time. The
			// producer must end its exact owned session before this frame's world processing, so
			// stop the terminal owned consumer through the retained exact-session authority even if
			// the weak caller backend already retired. This is ownership cleanup, not persistence or
			// a fabricated step, and it neither forces provider-loss Lost nor releases the clock.
			State.bHasFailure = true;
			State.LastFailure = Read;
			UE_LOG(LogCortexEditor, Display, TEXT("Recorded-frame clock step read failed (%s): %s"),
				*Read.ErrorCode, *Read.ErrorMessage);

			const FCortexCommandResult QuiesceResult =
				FCortexEditorPhysicalInputSession::QuiesceRetainedOwnedAuthority();
			if (!QuiesceResult.bSuccess)
			{
				UE_LOG(LogCortexEditor, Warning,
					TEXT("Terminal owned-consumer quiescence after a producer failure reported: %s"),
					*QuiesceResult.ErrorMessage);
			}

			return false;
		}

		// --- exact native validation: no tolerance, no clamp, no normalization ---------------
		if (!FMath::IsFinite(Step.CurrentSeconds) || !FMath::IsFinite(Step.LastSeconds)
			|| !FMath::IsFinite(Step.DeltaSeconds))
		{
			return FailStep(State, ClockLeaseCodes::TimingMismatch, TEXT("non_finite_step"));
		}
		if (Step.DeltaSeconds <= 0.0)
		{
			return FailStep(State, ClockLeaseCodes::TimingMismatch, TEXT("non_positive_delta"));
		}
		if (Step.CurrentSeconds <= Step.LastSeconds)
		{
			return FailStep(State, ClockLeaseCodes::TimingMismatch,
				FString::Printf(TEXT("non_monotonic_span current=%.9f last=%.9f"),
					Step.CurrentSeconds, Step.LastSeconds));
		}

		const double PriorCurrent = FApp::GetCurrentTime();
		if (Step.LastSeconds != PriorCurrent)
		{
			return FailStep(State, ClockLeaseCodes::TimingMismatch,
				FString::Printf(TEXT("step_last_mismatch expected=%.9f actual=%.9f"), PriorCurrent, Step.LastSeconds));
		}

		const double WallNow = FPlatformTime::Seconds();
		if (Step.CurrentSeconds > WallNow)
		{
			return FailStep(State, ClockLeaseCodes::PacingViolation,
				FString::Printf(TEXT("app_current_ahead_of_wall current=%.9f wall=%.9f"), Step.CurrentSeconds, WallNow));
		}

		// Apply the prepared step exactly: LastTime tracks the prior application CurrentTime and
		// the Current span stays independent of the recorded Delta.
		FApp::UpdateLastTime();
		FApp::SetCurrentTime(Step.CurrentSeconds);
		FApp::SetDeltaTime(Step.DeltaSeconds);

		// Exact native readback of what the engine actually holds.
		if (FApp::GetLastTime() != Step.LastSeconds
			|| FApp::GetCurrentTime() != Step.CurrentSeconds
			|| FApp::GetDeltaTime() != Step.DeltaSeconds)
		{
			return FailStep(State, ClockLeaseCodes::TimingMismatch,
				FString::Printf(TEXT("app_readback_mismatch last=%.9f current=%.9f delta=%.9f"),
					FApp::GetLastTime(), FApp::GetCurrentTime(), FApp::GetDeltaTime()));
		}

		return false;
	}

	bool UpdateTimeStepImpl(FClockState& State, UEngine& Engine, UCortexEditorRecordedFrameClock& Clock)
	{
		if (State.State == ECortexEditorClockState::Released)
		{
			return true;
		}
		if (State.State == ECortexEditorClockState::Lost)
		{
			// The lease no longer owns time. A cleared provider needs the engine's own normal
			// branch for its one-frame reanchor; a successor's frame must not get it.
			return Engine.GetCustomTimeStep() == nullptr;
		}
		if (State.State == ECortexEditorClockState::Handoff)
		{
			if (State.bCleanupStarted)
			{
				return Engine.GetCustomTimeStep() == nullptr;
			}
			return RunCleanupFrame(State, Engine, Clock);
		}

		if (Engine.GetCustomTimeStep() != &Clock)
		{
			LatchLost(State, ClockLeaseCodes::AttachmentLost,
				TEXT("the engine provider changed while the lease owned time"));
			return Engine.GetCustomTimeStep() == nullptr;
		}
		return RunTimedFrame(State, Engine, Clock);
	}
}

/**
 * Last-resort restore for a state destroyed while a cleanup frame was armed (or while any owned
 * fixed-step edit was installed): never leave this lease's engine-global edits behind. Delegates to
 * RestoreOwnedSettings, which only rewrites values that are still exactly ours.
 */
void RestoreOwnedSettingsOnStateDestroyed(FCortexEditorEngineClockLeaseState& State)
{
	RestoreOwnedSettings(State);
}

// ---------------------------------------------------------------------------
// UCortexEditorRecordedFrameClock
// ---------------------------------------------------------------------------

bool UCortexEditorRecordedFrameClock::Initialize(UEngine* InEngine)
{
	const TSharedPtr<FCortexEditorEngineClockLeaseState> Pinned = LeaseState.Pin();
	if (!Pinned.IsValid() || InEngine == nullptr || Pinned->Engine.Get() != InEngine)
	{
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (Pinned->bForceInitializeFailureForTests)
	{
		Pinned->bForceInitializeFailureForTests = false;
		return false;
	}
#endif
	return true;
}

void UCortexEditorRecordedFrameClock::Shutdown(UEngine* /*InEngine*/)
{
	// Owned resources are released only by the owner (owned handoff detach or a latched Lost),
	// never from an engine callback. This keeps a strong owner alive through a reentrant
	// Shutdown and never frees state a successor or callback is still reading.
}

bool UCortexEditorRecordedFrameClock::UpdateTimeStep(UEngine* InEngine)
{
	const TSharedPtr<FCortexEditorEngineClockLeaseState> Pinned = LeaseState.Pin();
	if (!Pinned.IsValid() || InEngine == nullptr)
	{
		// Owner gone: do not invent a frame; hand control to the engine's own clock.
		return true;
	}

	// Independent execution ownership: record that the engine is executing ON this clock, so a
	// retirement arriving from inside the callback cannot detach/release the object mid-call.
	// ReleaseOwnedResources defers the real release to a later tick while this depth is nonzero.
	struct FExecutionDepthScope
	{
		int32& Depth;
		explicit FExecutionDepthScope(int32& InDepth) : Depth(InDepth) { ++Depth; }
		~FExecutionDepthScope() { --Depth; }
	} ExecutionScope(Pinned->ExecutionDepth);

	return UpdateTimeStepImpl(*Pinned, *InEngine, *this);
}

ECustomTimeStepSynchronizationState UCortexEditorRecordedFrameClock::GetSynchronizationState() const
{
	const TSharedPtr<FCortexEditorEngineClockLeaseState> Pinned = LeaseState.Pin();
	if (!Pinned.IsValid())
	{
		return ECustomTimeStepSynchronizationState::Closed;
	}
	switch (Pinned->State)
	{
	case ECortexEditorClockState::Preparing:
	case ECortexEditorClockState::Replaying:
	case ECortexEditorClockState::Handoff:
		return ECustomTimeStepSynchronizationState::Synchronized;
	case ECortexEditorClockState::Lost:
		return ECustomTimeStepSynchronizationState::Error;
	case ECortexEditorClockState::Released:
	default:
		return ECustomTimeStepSynchronizationState::Closed;
	}
}

FString UCortexEditorRecordedFrameClock::GetDisplayName() const
{
	return TEXT("Cortex recorded-frame clock");
}

// ---------------------------------------------------------------------------
// FCortexEditorEngineClockLease
// ---------------------------------------------------------------------------

FCortexEditorEngineClockLease::FCortexEditorEngineClockLease() = default;

FCortexEditorEngineClockLease::~FCortexEditorEngineClockLease()
{
	if (State.IsValid())
	{
		// Last-resort safety cleanup only: detach the owned clock and restore only unchanged
		// owned settings. This never claims Released, so a destructor cannot fake a handoff.
		ReleaseOwnedResources(*State, /*bRestoreSettings*/ true);
		State.Reset();
	}
}

FCortexCommandResult FCortexEditorEngineClockLease::Acquire(UEngine& Engine, uint64 OperationGeneration,
	TFunction<FCortexCommandResult(FCortexEditorAppClockStep&)>&& ReadNextStep)
{
	if (!IsInGameThread())
	{
		return MakeClockLeaseError(ClockLeaseCodes::NotGameThread,
			TEXT("The recorded-frame clock lease may only be acquired on the Game Thread"));
	}

	if (State.IsValid() && State->State != ECortexEditorClockState::Released && State->State != ECortexEditorClockState::Lost)
	{
		return MakeClockLeaseError(ClockLeaseCodes::AlreadyHeld,
			TEXT("This clock lease owner already holds an active lease"));
	}

	if (!ReadNextStep)
	{
		return MakeClockLeaseError(CortexErrorCodes::InvalidOperation,
			TEXT("Acquire requires a ReadNextStep callback"));
	}

	if (!CortexEngineCompat::SupportsScopedPIE(Engine))
	{
		return MakeClockLeaseError(CortexErrorCodes::InvalidOperation,
			TEXT("Recorded-frame playback requires the ownership-scoped PIE engine capability version 1; stock engines are unsupported"));
	}

#if !defined(WITH_FIXED_TIME_STEP_SUPPORT) || !WITH_FIXED_TIME_STEP_SUPPORT
	return MakeClockLeaseError(ClockLeaseCodes::UnsupportedPlatform,
		TEXT("This build has no fixed-time-step support, so no truthful clock handoff exists"));
#else
	// --- timing-profile prerequisites (exact, source-backed) -------------------------------
	if (HasUnrelatedPIESession())
	{
		return MakeClockLeaseError(CortexErrorCodes::EditorBusy,
			TEXT("Another PIE session is active or pending; the recorded-frame clock will not disturb it"),
			TEXT("foreign_pie_session"));
	}
	if (Engine.GetCustomTimeStep() != nullptr)
	{
		return MakeClockLeaseError(ClockLeaseCodes::IncompatiblePrerequisites,
			TEXT("The engine already has a custom time step; the lease never overwrites a timing provider"),
			TEXT("foreign_custom_time_step"));
	}
	if (FApp::IsBenchmarking())
	{
		return MakeClockLeaseError(ClockLeaseCodes::IncompatiblePrerequisites,
			TEXT("Benchmarking/fixed application timestep is active"),
			TEXT("benchmarking"));
	}
	if (FApp::UseFixedTimeStep())
	{
		return MakeClockLeaseError(ClockLeaseCodes::IncompatiblePrerequisites,
			TEXT("A fixed application time step is enabled"),
			TEXT("fixed_time_step"));
	}
	if (Engine.bUseFixedFrameRate)
	{
		return MakeClockLeaseError(ClockLeaseCodes::IncompatiblePrerequisites,
			TEXT("A fixed engine framerate is enabled"),
			TEXT("fixed_frame_rate"));
	}
	if (IsOverrideFpsActive())
	{
		return MakeClockLeaseError(ClockLeaseCodes::IncompatiblePrerequisites,
			TEXT("An explicit application delta override is active"),
			TEXT("delta_override"));
	}

	TSharedPtr<FClockState> NewState = MakeShared<FClockState>();
	NewState->WeakSelf = NewState;
	NewState->Engine = &Engine;
	NewState->OperationGeneration = OperationGeneration;
	NewState->ReadNextStep = MoveTemp(ReadNextStep);
	NewState->State = ECortexEditorClockState::Preparing;
#if WITH_DEV_AUTOMATION_TESTS
	NewState->bForceInitializeFailureForTests = bForceInitializeFailureForTests;
	bForceInitializeFailureForTests = false;
#endif

	// Snapshot the exact profile we assume; only values still matching are ever restored.
	NewState->bOriginalUseFixedFrameRate = !!Engine.bUseFixedFrameRate;
	NewState->bOriginalUseFixedTimeStep = FApp::UseFixedTimeStep();
	NewState->OriginalFixedDeltaTime = FApp::GetFixedDeltaTime();
	NewState->bOriginalBenchmarking = FApp::IsBenchmarking();
	NewState->bOriginalOverrideFps = IsOverrideFpsActive();

	UCortexEditorRecordedFrameClock* const RawClock = NewObject<UCortexEditorRecordedFrameClock>(
		GetTransientPackage(), NAME_None, RF_Transient);
	if (RawClock == nullptr)
	{
		return MakeClockLeaseError(ClockLeaseCodes::InitializeFailed,
			TEXT("Could not allocate the transient recorded-frame clock"));
	}
	NewState->Clock = TStrongObjectPtr<UCortexEditorRecordedFrameClock>(RawClock);
	RawClock->LeaseState = NewState;

	RegisterProviderObserver(*NewState);

	// Publish before the external attachment callback so a reentrant call observes the lease.
	State = NewState;

	const bool bInitialized = Engine.SetCustomTimeStep(RawClock);
	UEngineCustomTimeStep* const Attached = Engine.GetCustomTimeStep();

	if (!bInitialized || Attached != RawClock)
	{
		const bool bOwnedAttached = (Attached == RawClock);
		if (bOwnedAttached)
		{
			// Initialize may have failed while leaving a non-null attached pointer; release only
			// that partial owned attachment, never a foreign successor.
			NewState->bOwnDetachInProgress = true;
			Engine.SetCustomTimeStep(nullptr);
			NewState->bOwnDetachInProgress = false;
		}
		ReleaseOwnedResources(*NewState, /*bRestoreSettings*/ false);
		State.Reset();
		return bInitialized
			? MakeClockLeaseError(ClockLeaseCodes::AttachmentLost,
				TEXT("The engine provider is not the recorded-frame clock after attachment"))
			: MakeClockLeaseError(ClockLeaseCodes::InitializeFailed,
				TEXT("The recorded-frame clock failed to initialize as the engine custom time step"));
	}

	UE_LOG(LogCortexEditor, Log, TEXT("Recorded-frame clock lease acquired (generation %llu)"),
		NewState->OperationGeneration);
	return MakeSuccess();
#endif
}

void FCortexEditorEngineClockLease::SetReplaying()
{
	if (State.IsValid() && State->State == ECortexEditorClockState::Preparing)
	{
		State->State = ECortexEditorClockState::Replaying;
	}
}

FCortexCommandResult FCortexEditorEngineClockLease::BeginHandoff()
{
	if (!IsInGameThread())
	{
		return MakeClockLeaseError(ClockLeaseCodes::NotGameThread,
			TEXT("Clock handoff may only be driven on the Game Thread"));
	}
	if (!State.IsValid())
	{
		return MakeClockLeaseError(ClockLeaseCodes::NotHeld, TEXT("No recorded-frame clock lease is held"));
	}

	FClockState& Current = *State;
	if (Current.State == ECortexEditorClockState::Released
		|| Current.State == ECortexEditorClockState::Lost
		|| Current.State == ECortexEditorClockState::Handoff)
	{
		// Idempotent: a completed, lost or already-pending handoff never runs another frame.
		return MakeSuccess();
	}

	// The matching owned PIE must have fully ended before any handoff side effect. Refusing here
	// keeps the clock cleanup from ever running against a live or queued PIE world.
	if (HasActivePIEWorld())
	{
		return MakeClockLeaseError(CortexErrorCodes::EditorBusy,
			TEXT("A PIE session is still active or queued; the clock handoff only follows a fully ended owned PIE"),
			TEXT("active_pie_world"));
	}

	Current.State = ECortexEditorClockState::Handoff;
	Current.bCleanupStarted = false;
	return MakeSuccess();
}

bool FCortexEditorEngineClockLease::IsHandoffComplete() const
{
	return State.IsValid() && State->State == ECortexEditorClockState::Released;
}

ECortexEditorClockState FCortexEditorEngineClockLease::GetState() const
{
	return State.IsValid() ? State->State : ECortexEditorClockState::Released;
}

FCortexCommandResult FCortexEditorEngineClockLease::GetLastFailure() const
{
	return State.IsValid() ? State->LastFailure : FCortexCommandResult();
}
