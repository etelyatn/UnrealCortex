#include "CortexEditorPIEState.h"
#include "CortexEditorModule.h"
#include "Containers/Ticker.h"
#include "Editor.h"

FCortexEditorPIEState::FCortexEditorPIEState()
{
}

FCortexEditorPIEState::~FCortexEditorPIEState()
{
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

void FCortexEditorPIEState::RegisterPendingCallback(FDeferredResponseCallback&& Callback)
{
	PendingCallbacks.Add(MoveTemp(Callback));
}

void FCortexEditorPIEState::CompletePendingCallbacks(const FCortexCommandResult& Result)
{
	for (FDeferredResponseCallback& Callback : PendingCallbacks)
	{
		Callback(Result);
	}
	PendingCallbacks.Empty();
}

void FCortexEditorPIEState::OnPIEEnded()
{
	SetState(ECortexPIEState::Stopped);

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
