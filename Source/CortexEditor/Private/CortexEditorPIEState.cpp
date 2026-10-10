#include "CortexEditorPIEState.h"
#include "CortexEditorModule.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"

namespace
{
FCortexCommandResult MakeContinuousInputCancellationResult()
{
	FCortexCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = TEXT("OperationCancelled");
	Result.ErrorMessage = TEXT("Continuous input injection cancelled");
	return Result;
}
}

FCortexEditorPIEState::FCortexEditorPIEState()
{
}

FCortexEditorPIEState::~FCortexEditorPIEState()
{
	CancelAllInputTickers();

	// If HandleCancelPIE deferred OnPIEEnded() but the ticker hasn't fired yet,
	// run cleanup now so pending callbacks are completed and state is consistent.
	if (CancelDeferHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(CancelDeferHandle);
		CancelDeferHandle.Reset();
		OnPIEEnded();
	}
	UnbindDelegates();
}

void FCortexEditorPIEState::BindDelegates()
{
	FEditorDelegates::PreBeginPIE.AddRaw(this, &FCortexEditorPIEState::HandlePrePIEStarted);
	FEditorDelegates::PostPIEStarted.AddRaw(this, &FCortexEditorPIEState::HandlePostPIEStarted);
	FEditorDelegates::PausePIE.AddRaw(this, &FCortexEditorPIEState::HandlePausePIE);
	FEditorDelegates::ResumePIE.AddRaw(this, &FCortexEditorPIEState::HandleResumePIE);
	FEditorDelegates::PrePIEEnded.AddRaw(this, &FCortexEditorPIEState::HandlePrePIEEnded);
	FEditorDelegates::EndPIE.AddRaw(this, &FCortexEditorPIEState::HandleEndPIE);
	FEditorDelegates::CancelPIE.AddRaw(this, &FCortexEditorPIEState::HandleCancelPIE);

	// Scoped owned ends are target-identifying: observe them so the owned input resources are retired
	// without touching a surviving foreign instance. Repository engines without the capability return
	// an invalid handle and register nothing, so the global path above remains authoritative there.
	ScopedLifecycleDelegateHandle = CortexEngineCompat::ObserveScopedPIELifecycle(
		[this](const FCortexScopedPIESnapshot& Snapshot)
		{
			HandleScopedPIELifecycle(Snapshot);
		});
}

void FCortexEditorPIEState::UnbindDelegates()
{
	FEditorDelegates::PreBeginPIE.RemoveAll(this);
	FEditorDelegates::PostPIEStarted.RemoveAll(this);
	FEditorDelegates::PausePIE.RemoveAll(this);
	FEditorDelegates::ResumePIE.RemoveAll(this);
	FEditorDelegates::PrePIEEnded.RemoveAll(this);
	FEditorDelegates::EndPIE.RemoveAll(this);
	FEditorDelegates::CancelPIE.RemoveAll(this);
	CortexEngineCompat::RemoveScopedPIELifecycleObserver(ScopedLifecycleDelegateHandle);
	ScopedLifecycleDelegateHandle.Reset();
}

void FCortexEditorPIEState::SetState(ECortexPIEState NewState)
{
	UE_LOG(LogCortexEditor, Log, TEXT("PIE state: %s -> %s"),
		*StateToString(State), *StateToString(NewState));
	State = NewState;
}

bool FCortexEditorPIEState::IsInTransition() const
{
	return State == ECortexPIEState::Starting || State == ECortexPIEState::Stopping;
}

bool FCortexEditorPIEState::IsActive() const
{
	return State == ECortexPIEState::Playing || State == ECortexPIEState::Paused;
}

FString FCortexEditorPIEState::StateToString(ECortexPIEState InState)
{
	switch (InState)
	{
	case ECortexPIEState::Stopped:
		return TEXT("stopped");
	case ECortexPIEState::Starting:
		return TEXT("starting");
	case ECortexPIEState::Playing:
		return TEXT("playing");
	case ECortexPIEState::Paused:
		return TEXT("paused");
	case ECortexPIEState::Stopping:
		return TEXT("stopping");
	default:
		return TEXT("unknown");
	}
}

uint32 FCortexEditorPIEState::RegisterPendingCallback(FDeferredResponseCallback&& Callback)
{
	const uint32 Id = ++NextCallbackId;
	PendingCallbacks.Add(Id, MoveTemp(Callback));
	return Id;
}

void FCortexEditorPIEState::CompletePendingCallback(uint32 CallbackId, const FCortexCommandResult& Result)
{
	FDeferredResponseCallback Callback;
	if (PendingCallbacks.RemoveAndCopyValue(CallbackId, Callback))
	{
		Callback(Result);
	}
}

void FCortexEditorPIEState::CompletePendingCallbacks(const FCortexCommandResult& Result)
{
	TMap<uint32, FDeferredResponseCallback> CallbacksCopy = MoveTemp(PendingCallbacks);
	for (TPair<uint32, FDeferredResponseCallback>& Pair : CallbacksCopy)
	{
		Pair.Value(Result);
	}
}

uint32 FCortexEditorPIEState::RegisterPendingInputCallback(FDeferredResponseCallback&& Callback)
{
	const uint32 Id = ++NextInputCallbackId;
	PendingInputCallbacks.Add(Id, MoveTemp(Callback));
	return Id;
}

void FCortexEditorPIEState::CompletePendingInputCallback(uint32 CallbackId, const FCortexCommandResult& Result)
{
	FDeferredResponseCallback Callback;
	if (PendingInputCallbacks.RemoveAndCopyValue(CallbackId, Callback))
	{
		Callback(Result);
	}
}

void FCortexEditorPIEState::CompletePendingInputCallbacks(const FCortexCommandResult& Result)
{
	TMap<uint32, FDeferredResponseCallback> CallbacksCopy = MoveTemp(PendingInputCallbacks);
	for (TPair<uint32, FDeferredResponseCallback>& Pair : CallbacksCopy)
	{
		Pair.Value(Result);
	}
}

void FCortexEditorPIEState::RegisterInputTickerHandle(FTSTicker::FDelegateHandle Handle)
{
	if (Handle.IsValid())
	{
		InputTickerHandles.Add(Handle);
	}
}

void FCortexEditorPIEState::CancelAllInputTickers()
{
	// A cancellation callback must not be able to install a successor run that this cleanup would
	// not own; block all new input admission for the duration of the teardown.
	TGuardValue<bool> AdmissionBlock(bInputAdmissionBlocked, true);

	*InputCancelToken = true;
	InputCancelToken = MakeShared<FThreadSafeBool>(false);

	// Move owned state out before anything can invoke a deferred response: completing a callback
	// may re-enter the session and mutate the containers we would otherwise be iterating.
	TMap<TWeakObjectPtr<const UInputAction>, FContinuousInputRun> RunsToStop = MoveTemp(ContinuousInputRuns);
	TArray<FTSTicker::FDelegateHandle> TickersToRemove = MoveTemp(InputTickerHandles);
	const bool bHadInputTickers = TickersToRemove.Num() > 0;

	// Owned native injections must be stopped synchronously before their tickers are removed,
	// otherwise a timed or indefinite Cortex injection would outlive its owner.
	for (TPair<TWeakObjectPtr<const UInputAction>, FContinuousInputRun>& Pair : RunsToStop)
	{
		StopNativeContinuousInjection(Pair.Value);
	}

	for (FTSTicker::FDelegateHandle& Handle : TickersToRemove)
	{
		if (Handle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Handle);
			Handle.Reset();
		}
	}

	// Complete each owned timed run's pending caller exactly once with a single shared cancellation.
	const FCortexCommandResult Cancellation = MakeContinuousInputCancellationResult();
	for (TPair<TWeakObjectPtr<const UInputAction>, FContinuousInputRun>& Pair : RunsToStop)
	{
		if (Pair.Value.bHasCallback)
		{
			CompletePendingInputCallback(Pair.Value.CallbackId, Cancellation);
		}
	}

	// Any still-pending input callback belongs to a session that is being cancelled: complete it
	// exactly once, even when its owned tickers/runs were already retired by an earlier scoped end
	// (the second call is a no-op because the map is empty by then).
	if (bHadInputTickers || RunsToStop.Num() > 0 || PendingInputCallbacks.Num() > 0)
	{
		CompletePendingInputCallbacks(Cancellation);
	}
}

void FCortexEditorPIEState::StopNativeContinuousInjection(const FContinuousInputRun& Run)
{
	UEnhancedInputLocalPlayerSubsystem* Subsystem = Run.Subsystem.Get();
	const UInputAction* Action = Run.Action.Get();
	if (Subsystem != nullptr && Action != nullptr)
	{
		Subsystem->StopContinuousInputInjectionForAction(Action);
	}
}

bool FCortexEditorPIEState::InvalidateContinuousInputRun(const UInputAction* Action, bool& bCancelledDuringCallback)
{
	bCancelledDuringCallback = false;

	// The predecessor's cancellation callback runs inside this function; block new admission so it
	// cannot install an intervening run before the caller's successor is tracked.
	TGuardValue<bool> AdmissionBlock(bInputAdmissionBlocked, true);

	if (Action == nullptr)
	{
		return false;
	}

	const TWeakObjectPtr<const UInputAction> Key(Action);
	FContinuousInputRun* Existing = ContinuousInputRuns.Find(Key);
	if (Existing == nullptr)
	{
		return false;
	}

	FContinuousInputRun Run = MoveTemp(*Existing);
	ContinuousInputRuns.Remove(Key);

	// Native resources are released before their stop timer, matching CancelAllInputTickers.
	StopNativeContinuousInjection(Run);

	if (Run.TimerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Run.TimerHandle);
		InputTickerHandles.Remove(Run.TimerHandle);
	}

	if (!Run.bHasCallback)
	{
		return false;
	}

	// The callback may synchronously disconnect or end PIE, which cancels the session by flipping
	// the token this shared ref still points at; the caller revalidates before mutating.
	const TSharedRef<FThreadSafeBool> TokenBeforeCallback = InputCancelToken;
	CompletePendingInputCallback(Run.CallbackId, MakeContinuousInputCancellationResult());
	bCancelledDuringCallback = *TokenBeforeCallback;
	return true;
}

bool FCortexEditorPIEState::OwnsContinuousInputRunForSubsystem(
	const UInputAction* Action,
	const UEnhancedInputLocalPlayerSubsystem* Subsystem) const
{
	if (Action == nullptr || Subsystem == nullptr)
	{
		return false;
	}

	const FContinuousInputRun* Run = ContinuousInputRuns.Find(TWeakObjectPtr<const UInputAction>(Action));
	return Run != nullptr && Run->Subsystem.Get() == Subsystem;
}

bool FCortexEditorPIEState::StopOwnedContinuousInputRun(const UInputAction* Action)
{
	if (Action == nullptr || !ContinuousInputRuns.Contains(TWeakObjectPtr<const UInputAction>(Action)))
	{
		return false;
	}

	bool bCancelledDuringCallback = false;
	InvalidateContinuousInputRun(Action, bCancelledDuringCallback);
	return true;
}

uint32 FCortexEditorPIEState::TrackContinuousInputRun(
	UEnhancedInputLocalPlayerSubsystem* Subsystem,
	const UInputAction* Action,
	bool bTimed,
	float DelaySeconds,
	bool bHasCallback,
	uint32 CallbackId)
{
	const TWeakObjectPtr<const UInputAction> Key(Action);
	FContinuousInputRun& Run = ContinuousInputRuns.FindOrAdd(Key);
	Run.Subsystem = Subsystem;
	Run.Action = Action;
	Run.Generation = ++ContinuousInputRunGeneration;
	Run.bHasCallback = bHasCallback && bTimed;
	Run.CallbackId = CallbackId;
	Run.TimerHandle.Reset();

	if (bTimed)
	{
		const uint32 Generation = Run.Generation;
		const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this, Key, Generation](float) -> bool
			{
				HandleContinuousInputTimerElapsed(Key, Generation);
				return false;
			}),
			DelaySeconds);
		Run.TimerHandle = Handle;
		RegisterInputTickerHandle(Handle);
	}

	return Run.Generation;
}

void FCortexEditorPIEState::HandleContinuousInputTimerElapsed(
	const TWeakObjectPtr<const UInputAction>& ActionKey,
	uint32 Generation)
{
	FContinuousInputRun* Existing = ContinuousInputRuns.Find(ActionKey);
	if (Existing == nullptr || Existing->Generation != Generation)
	{
		// A stale stop timer must never touch the run that succeeded it.
		return;
	}

	FContinuousInputRun Run = MoveTemp(*Existing);
	ContinuousInputRuns.Remove(ActionKey);

	// One-shot ticker is being removed by the ticker system as we run; drop our bookkeeping
	// handle and stop the injection the timer was guarding.
	InputTickerHandles.Remove(Run.TimerHandle);
	StopNativeContinuousInjection(Run);

	if (Run.bHasCallback)
	{
		FCortexCommandResult Result;
		Result.bSuccess = true;
		Result.Data = MakeShared<FJsonObject>();
		Result.Data->SetBoolField(TEXT("injecting"), false);
		CompletePendingInputCallback(Run.CallbackId, Result);
	}
}

void FCortexEditorPIEState::OnPIEEnded()
{
	CancelAllInputTickers();
	SetState(ECortexPIEState::Stopped);

	// Input callbacks already completed with OperationCancelled by CancelAllInputTickers() above.
	// Only general PIE callbacks (start_pie, stop_pie) need PIETerminated here.
	FCortexCommandResult ErrorResult;
	ErrorResult.bSuccess = false;
	ErrorResult.ErrorCode = CortexErrorCodes::PIETerminated;
	ErrorResult.ErrorMessage = TEXT("PIE session ended while command was pending");
	CompletePendingCallbacks(ErrorResult);
}

void FCortexEditorPIEState::CompletePendingSuccess()
{
	FCortexCommandResult SuccessResult;
	SuccessResult.bSuccess = true;
	SuccessResult.Data = MakeShared<FJsonObject>();
	SuccessResult.Data->SetStringField(TEXT("state"), StateToString(State));
	CompletePendingCallbacks(SuccessResult);
}

void FCortexEditorPIEState::HandlePrePIEStarted(bool bIsSimulating)
{
	(void)bIsSimulating;
	SetState(ECortexPIEState::Starting);
}

void FCortexEditorPIEState::HandlePostPIEStarted(bool bIsSimulating)
{
	(void)bIsSimulating;
	SetState(ECortexPIEState::Playing);
	CompletePendingSuccess();
}

void FCortexEditorPIEState::HandlePausePIE(bool bIsSimulating)
{
	(void)bIsSimulating;
	SetState(ECortexPIEState::Paused);
}

void FCortexEditorPIEState::HandleResumePIE(bool bIsSimulating)
{
	(void)bIsSimulating;
	SetState(ECortexPIEState::Playing);
}

void FCortexEditorPIEState::HandlePrePIEEnded(bool bIsSimulating)
{
	(void)bIsSimulating;
	SetState(ECortexPIEState::Stopping);
}

void FCortexEditorPIEState::HandleEndPIE(bool bIsSimulating)
{
	(void)bIsSimulating;

	// Cancel input tickers early so they cannot fire during PIE teardown.
	// OnPIEEnded() calls CancelAllInputTickers() again, but that is a safe no-op
	// (empty handles array, bHadInputTickers is false, fresh token already set).
	CancelAllInputTickers();

	if (State == ECortexPIEState::Stopping)
	{
		SetState(ECortexPIEState::Stopped);
		CompletePendingSuccess();
		return;
	}
	OnPIEEnded();
}

void FCortexEditorPIEState::HandleCancelPIE()
{
	UE_LOG(LogCortexEditor, Log, TEXT("PIE cancelled"));

	// Do not call OnPIEEnded() synchronously here.
	// CancelPIE can fire from inside RequestPlaySession() or CancelRequestPlaySession()
	// while the engine holds internal locks.  Calling UE_LOG from within that call
	// stack (via SetState -> UE_LOG inside OnPIEEnded) deadlocks against those locks.
	// Defer to the next tick so we are safely outside the engine call.
	CancelDeferHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) mutable
		{
			CancelDeferHandle.Reset();
			OnPIEEnded();
			return false;
		}));
}

void FCortexEditorPIEState::HandleScopedPIELifecycle(const FCortexScopedPIESnapshot& Snapshot)
{
	// Only the owned instance's ending retires owned resources; the Ended transition needs nothing
	// here, and non-ending phases are ignored.
	if (Snapshot.Phase != ECortexScopedPIEPhase::Ending)
	{
		return;
	}

	// Deliberately does NOT change the global state machine: with a surviving foreign instance the
	// editor is still in PIE, and only the engine's final/global end may report PIE stopped.
	CancelOwnedInputForScopedEnd();
}

void FCortexEditorPIEState::CancelOwnedInputForScopedEnd()
{
	// A cancellation callback must not install a successor run this cleanup would not own.
	TGuardValue<bool> AdmissionBlock(bInputAdmissionBlocked, true);

	// Move owned state out before anything can invoke a deferred response: completing a callback may
	// re-enter the session and mutate the containers we would otherwise be iterating.
	TMap<TWeakObjectPtr<const UInputAction>, FContinuousInputRun> RunsToStop = MoveTemp(ContinuousInputRuns);
	TArray<FTSTicker::FDelegateHandle> TickersToRemove = MoveTemp(InputTickerHandles);

	// Owned native injections are stopped synchronously before their tickers are removed, so an
	// owned continuous injection can never outlive its owner.
	for (TPair<TWeakObjectPtr<const UInputAction>, FContinuousInputRun>& Pair : RunsToStop)
	{
		StopNativeContinuousInjection(Pair.Value);
	}

	for (FTSTicker::FDelegateHandle& Handle : TickersToRemove)
	{
		if (Handle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Handle);
			Handle.Reset();
		}
	}

	// Each owned timed run's pending caller completes exactly once with a shared cancellation.
	const FCortexCommandResult Cancellation = MakeContinuousInputCancellationResult();
	for (TPair<TWeakObjectPtr<const UInputAction>, FContinuousInputRun>& Pair : RunsToStop)
	{
		if (Pair.Value.bHasCallback)
		{
			CompletePendingInputCallback(Pair.Value.CallbackId, Cancellation);
		}
	}
}
