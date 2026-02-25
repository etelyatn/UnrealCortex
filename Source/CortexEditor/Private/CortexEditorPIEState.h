#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "Containers/Ticker.h"
#include "HAL/ThreadSafeBool.h"

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
	void RegisterInputTickerHandle(FTSTicker::FDelegateHandle Handle);
	void CancelAllInputTickers();
	TSharedRef<FThreadSafeBool> GetInputCancelToken() const { return InputCancelToken; }
	void OnPIEEnded();

private:
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
	uint32 NextCallbackId = 0;

	// Handle for the deferred OnPIEEnded() ticker scheduled by HandleCancelPIE().
	// Calling UE_LOG from within a CancelPIE delegate broadcast that is itself fired
	// inside RequestPlaySession() or CancelRequestPlaySession() can deadlock with
	// engine-internal locks.  We defer state cleanup to the next tick instead.
	FTSTicker::FDelegateHandle CancelDeferHandle;
	TArray<FTSTicker::FDelegateHandle> InputTickerHandles;
	TSharedRef<FThreadSafeBool> InputCancelToken = MakeShared<FThreadSafeBool>(false);
};
