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
	void OnPIEEnded();

private:
	void HandlePrePIEStarted(bool bIsSimulating);
	void HandlePostPIEStarted(bool bIsSimulating);
	void HandlePausePIE(bool bIsSimulating);
	void HandleResumePIE(bool bIsSimulating);
	void HandlePrePIEEnded(bool bIsSimulating);
	void HandleEndPIE(bool bIsSimulating);
	void HandleCancelPIE();

	ECortexPIEState State = ECortexPIEState::Stopped;
	TArray<FDeferredResponseCallback> PendingCallbacks;
};
