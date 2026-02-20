#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

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

	void RegisterPendingCallback(FDeferredResponseCallback&& Callback);
	void CompletePendingCallbacks(const FCortexCommandResult& Result);
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
	TArray<FDeferredResponseCallback> PendingCallbacks;

	// Handle for the deferred OnPIEEnded() ticker scheduled by HandleCancelPIE().
	// Calling UE_LOG from within a CancelPIE delegate broadcast that is itself fired
	// inside RequestPlaySession() or CancelRequestPlaySession() can deadlock with
	// engine-internal locks.  We defer state cleanup to the next tick instead.
	FTSTicker::FDelegateHandle CancelDeferHandle;
};
