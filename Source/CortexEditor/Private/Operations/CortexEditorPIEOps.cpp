#include "Operations/CortexEditorPIEOps.h"
#include "CortexEditorPIEState.h"
#include "CortexCommandRouter.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "LevelEditor.h"
#include "Containers/Ticker.h"

FCortexCommandResult FCortexEditorPIEOps::GetPIEState(const FCortexEditorPIEState& PIEState)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("state"), FCortexEditorPIEState::StateToString(PIEState.GetState()));
	Data->SetBoolField(TEXT("is_active"), PIEState.IsActive());
	Data->SetBoolField(TEXT("is_transition"), PIEState.IsInTransition());
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorPIEOps::StartPIE(
	FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	if (GEditor == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Editor is not ready"));
	}

	if (PIEState.IsInTransition())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIETransitionInProgress,
			TEXT("PIE is currently starting/stopping. Wait and retry."));
	}
	if (PIEState.IsActive())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIEAlreadyActive,
			TEXT("PIE is already running. Call stop_pie or restart_pie."));
	}

	if (DeferredCallback)
	{
		PIEState.RegisterPendingCallback(MoveTemp(DeferredCallback));
	}

	FRequestPlaySessionParams RequestParams;
	FString Mode = TEXT("selected_viewport");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("mode"), Mode);
	}
	if (Mode == TEXT("selected_viewport"))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::Get().GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		RequestParams.DestinationSlateViewport = LevelEditorModule.GetFirstActiveViewport();
	}

	PIEState.SetState(ECortexPIEState::Starting);
	GEditor->RequestPlaySession(RequestParams);

	FCortexCommandResult Deferred;
	Deferred.bIsDeferred = true;
	return Deferred;
}

FCortexCommandResult FCortexEditorPIEOps::StopPIE(
	FCortexEditorPIEState& PIEState,
	FDeferredResponseCallback DeferredCallback)
{
	if (GEditor == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Editor is not ready"));
	}

	if (PIEState.IsInTransition())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIETransitionInProgress,
			TEXT("PIE is currently starting/stopping. Wait and retry."));
	}
	if (!PIEState.IsActive())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}

	if (DeferredCallback)
	{
		PIEState.RegisterPendingCallback(MoveTemp(DeferredCallback));
	}

	PIEState.SetState(ECortexPIEState::Stopping);
	GEditor->RequestEndPlayMap();

	FCortexCommandResult Deferred;
	Deferred.bIsDeferred = true;
	return Deferred;
}

FCortexCommandResult FCortexEditorPIEOps::PausePIE(FCortexEditorPIEState& PIEState)
{
	if (GEditor == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Editor is not ready"));
	}

	if (!PIEState.IsActive())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}
	if (PIEState.GetState() == ECortexPIEState::Paused)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIEAlreadyPaused,
			TEXT("PIE is already paused."));
	}

	GEditor->SetPIEWorldsPaused(true);
	PIEState.SetState(ECortexPIEState::Paused);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("state"), TEXT("paused"));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorPIEOps::ResumePIE(FCortexEditorPIEState& PIEState)
{
	if (GEditor == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Editor is not ready"));
	}

	if (PIEState.GetState() != ECortexPIEState::Paused)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotPaused,
			TEXT("PIE is not paused."));
	}

	GEditor->SetPIEWorldsPaused(false);
	PIEState.SetState(ECortexPIEState::Playing);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("state"), TEXT("playing"));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorPIEOps::RestartPIE(
	FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	if (GEditor == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Editor is not ready"));
	}

	if (PIEState.IsInTransition())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIETransitionInProgress,
			TEXT("PIE is currently starting/stopping. Wait and retry."));
	}
	if (!PIEState.IsActive())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}

	PIEState.SetState(ECortexPIEState::Stopping);
	GEditor->RequestEndPlayMap();

	const double RestartStartTime = FPlatformTime::Seconds();
	FCortexEditorPIEState* PIEStatePtr = &PIEState;
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([PIEStatePtr, Params, Callback = MoveTemp(DeferredCallback), RestartStartTime](float DeltaTime) mutable
		{
			(void)DeltaTime;

			if (PIEStatePtr->GetState() == ECortexPIEState::Stopped)
			{
				const FCortexCommandResult StartResult = FCortexEditorPIEOps::StartPIE(*PIEStatePtr, Params, MoveTemp(Callback));
				if (!StartResult.bIsDeferred && Callback)
				{
					Callback(StartResult);
				}
				return false;
			}

			if ((FPlatformTime::Seconds() - RestartStartTime) > 10.0)
			{
				if (Callback)
				{
					FCortexCommandResult TimeoutResult = FCortexCommandRouter::Error(
						CortexErrorCodes::PIETerminated,
						TEXT("restart_pie timed out waiting for stop phase"));
					Callback(TimeoutResult);
				}
				return false;
			}

			return true;
		}),
		0.1f);

	FCortexCommandResult Deferred;
	Deferred.bIsDeferred = true;
	return Deferred;
}
