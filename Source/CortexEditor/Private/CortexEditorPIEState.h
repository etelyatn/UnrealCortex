#pragma once

#include "CoreMinimal.h"
#include "CortexEngineCompat.h"
#include "CortexTypes.h"
#include "Containers/Ticker.h"
#include "HAL/ThreadSafeBool.h"

class UEnhancedInputLocalPlayerSubsystem;
class UInputAction;

enum class ECortexPIEState : uint8
{
	Stopped,
	Starting,
	Playing,
	Paused,
	Stopping
};

class FCortexEditorPIEState
{
public:
	FCortexEditorPIEState();
	~FCortexEditorPIEState();

	void BindDelegates();
	void UnbindDelegates();

	ECortexPIEState GetState() const { return State; }
	void SetState(ECortexPIEState NewState);

	bool IsInTransition() const;
	bool IsActive() const;

	static FString StateToString(ECortexPIEState InState);

	uint32 RegisterPendingCallback(FDeferredResponseCallback&& Callback);
	void CompletePendingCallback(uint32 CallbackId, const FCortexCommandResult& Result);
	void CompletePendingCallbacks(const FCortexCommandResult& Result);
	uint32 RegisterPendingInputCallback(FDeferredResponseCallback&& Callback);
	void CompletePendingInputCallback(uint32 CallbackId, const FCortexCommandResult& Result);
	void CompletePendingInputCallbacks(const FCortexCommandResult& Result);
	void RegisterInputTickerHandle(FTSTicker::FDelegateHandle Handle);
	void CancelAllInputTickers();
	TSharedRef<FThreadSafeBool> GetInputCancelToken() const { return InputCancelToken; }
	void OnPIEEnded();

	// --- Session-owned continuous input injections -------------------------------------------
	// The PIE session owns each Cortex-started continuous Enhanced Input injection by weak
	// subsystem/action identity plus a monotone run generation. The owner is the only actor that
	// stops them, so replace/stop/teardown invalidation can never touch a successor run or a
	// game-managed injection.
	//
	// InvalidateContinuousInputRun stops the owned native injection, cancels its stop timer and
	// completes its pending deferred caller once as a cancellation. Safe when the action is unowned.
	// Returns true when a pending caller was invoked (the caller must then revalidate its target and
	// context before mutating). bCancelledDuringCallback is set true when the session was cancelled
	// (InputCancelToken flipped) by that callback, e.g. a disconnect from inside it.
	bool InvalidateContinuousInputRun(const UInputAction* Action, bool& bCancelledDuringCallback);
	// True when this session owns a live continuous run for the exact weak subsystem/action pair.
	bool OwnsContinuousInputRunForSubsystem(
		const UInputAction* Action,
		const UEnhancedInputLocalPlayerSubsystem* Subsystem) const;
	// Stops and forgets an owned run. Returns false when the action is unowned (nothing changed).
	bool StopOwnedContinuousInputRun(const UInputAction* Action);
	// Records a freshly started owned run. Callers must have invalidated the prior run first.
	// DelaySeconds is the already validated native float delay. Returns the new run generation.
	uint32 TrackContinuousInputRun(
		UEnhancedInputLocalPlayerSubsystem* Subsystem,
		const UInputAction* Action,
		bool bTimed,
		float DelaySeconds,
		bool bHasCallback,
		uint32 CallbackId);

	// True while owned runs/tickers are being torn down; new input admission is rejected so a
	// cancellation callback can never install a successor run the cleanup would not own.
	bool IsInputAdmissionBlocked() const { return bInputAdmissionBlocked; }

	// --- Scoped owned-end handling -------------------------------------------------------------
	// A scoped end retires only the owned instance; a foreign instance may still be running, so the
	// global state machine and the session-wide cancellation token/pending callbacks are left alone.
	// Public entry point so the engine capability's scoped lifecycle (and its tests) can drive it by
	// exact identity instead of the targetless global PIE events.
	void HandleScopedPIELifecycle(const FCortexScopedPIESnapshot& Snapshot);
	// Stops only this session's owned continuous injections/tickers and completes their own pending
	// callers. Deliberately neither flips InputCancelToken nor calls CompletePendingInputCallbacks:
	// those are session-wide and may belong to a surviving foreign owner.
	void CancelOwnedInputForScopedEnd();

private:
	struct FContinuousInputRun
	{
		TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> Subsystem;
		TWeakObjectPtr<const UInputAction> Action;
		uint32 Generation = 0;
		bool bHasCallback = false;
		uint32 CallbackId = 0;
		FTSTicker::FDelegateHandle TimerHandle;
	};

	void HandleContinuousInputTimerElapsed(const TWeakObjectPtr<const UInputAction>& ActionKey, uint32 Generation);
	void StopNativeContinuousInjection(const FContinuousInputRun& Run);

	void CompletePendingSuccess();

	void HandlePrePIEStarted(bool bIsSimulating);
	void HandlePostPIEStarted(bool bIsSimulating);
	void HandlePausePIE(bool bIsSimulating);
	void HandleResumePIE(bool bIsSimulating);
	void HandlePrePIEEnded(bool bIsSimulating);
	void HandleEndPIE(bool bIsSimulating);
	void HandleCancelPIE();

	ECortexPIEState State = ECortexPIEState::Stopped;
	TMap<uint32, FDeferredResponseCallback> PendingCallbacks;
	uint32 NextCallbackId = 0;  // Wraps at UINT32_MAX; collision unreachable in practice
	TMap<uint32, FDeferredResponseCallback> PendingInputCallbacks;
	uint32 NextInputCallbackId = 0;  // Wraps at UINT32_MAX; collision unreachable in practice

	// Handle for the deferred OnPIEEnded() ticker scheduled by HandleCancelPIE().
	// Calling UE_LOG from within a CancelPIE delegate broadcast that is itself fired
	// inside RequestPlaySession() or CancelRequestPlaySession() can deadlock with
	// engine-internal locks.  We defer state cleanup to the next tick instead.
	FTSTicker::FDelegateHandle CancelDeferHandle;
	TArray<FTSTicker::FDelegateHandle> InputTickerHandles;
	TSharedRef<FThreadSafeBool> InputCancelToken = MakeShared<FThreadSafeBool>(false);

	/** Subscription to the engine capability's scoped lifecycle; invalid on unsupported engines. */
	FDelegateHandle ScopedLifecycleDelegateHandle;

	// Per-action owned continuous injections; keyed by the actual (weak) action. Generation is
	// monotone across the session so a stale stop timer can never act on a successor run.
	TMap<TWeakObjectPtr<const UInputAction>, FContinuousInputRun> ContinuousInputRuns;
	uint32 ContinuousInputRunGeneration = 0;
	bool bInputAdmissionBlocked = false;
};
