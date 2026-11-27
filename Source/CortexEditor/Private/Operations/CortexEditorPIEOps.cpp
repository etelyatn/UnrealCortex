#include "Operations/CortexEditorPIEOps.h"
#include "CortexEditorPIEState.h"
#include "CortexEditorModule.h"
#include "CortexCommandRouter.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "LevelEditor.h"
#include "Containers/Ticker.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
/** PIE transitions report their result only through the deferred callback; without one the caller could never learn it. */
FCortexCommandResult MissingCallbackError(const TCHAR* Command)
{
	return FCortexCommandRouter::Error(
		CortexErrorCodes::InvalidOperation,
		FString::Printf(TEXT("%s requires a deferred response callback and cannot run inside core.batch"), Command));
}

enum class ECortexPIELaunchMode : uint8
{
	SelectedViewport,
	NewWindow,
};

/** Reads `mode`: selected_viewport (default) or new_window. Any other value is refused before PIE state changes. */
bool TryReadLaunchMode(const TSharedPtr<FJsonObject>& Params, ECortexPIELaunchMode& OutMode, FCortexCommandResult& OutError)
{
	OutMode = ECortexPIELaunchMode::SelectedViewport;
	if (!Params.IsValid() || !Params->HasField(TEXT("mode")))
	{
		return true;
	}

	FString Mode;
	if (!Params->TryGetStringField(TEXT("mode"), Mode))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("mode must be a string: selected_viewport or new_window"));
		return false;
	}
	if (Mode == TEXT("selected_viewport"))
	{
		return true;
	}
	if (Mode == TEXT("new_window"))
	{
		OutMode = ECortexPIELaunchMode::NewWindow;
		return true;
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::InvalidField,
		FString::Printf(TEXT("Invalid mode: %s (expected selected_viewport or new_window)"), *Mode));
	return false;
}
}

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
	ECortexPIELaunchMode LaunchMode = ECortexPIELaunchMode::SelectedViewport;
	FCortexCommandResult ModeError;
	if (!TryReadLaunchMode(Params, LaunchMode, ModeError))
	{
		return ModeError;
	}

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

	// Ground-truth check: if tracked state says active but PlayWorld is null,
	// the delegate-driven state is stale — reset to Stopped.
	if (PIEState.IsActive() && GEditor->PlayWorld == nullptr)
	{
		UE_LOG(LogCortexEditor, Warning,
			TEXT("PIE state tracker reported active (%s) but PlayWorld is null — resetting to Stopped"),
			*FCortexEditorPIEState::StateToString(PIEState.GetState()));
		PIEState.SetState(ECortexPIEState::Stopped);
	}

	if (PIEState.IsActive())
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("current_state"), FCortexEditorPIEState::StateToString(PIEState.GetState()));
		Data->SetBoolField(TEXT("is_active"), true);
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIEAlreadyActive,
			TEXT("PIE is already running. Call stop_pie or restart_pie."),
			Data);
	}

	if (!DeferredCallback)
	{
		return MissingCallbackError(TEXT("start_pie"));
	}
	// ID unused: PIE lifecycle completes all general callbacks via broadcast
	const uint32 CallbackId = PIEState.RegisterPendingCallback(MoveTemp(DeferredCallback));
	(void)CallbackId;

	FRequestPlaySessionParams RequestParams;
	if (LaunchMode == ECortexPIELaunchMode::SelectedViewport)
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::Get().GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		RequestParams.DestinationSlateViewport = LevelEditorModule.GetFirstActiveViewport();
	}
	else
	{
		// Floating PIE window: no destination viewport, so the engine creates its own window. The play mode
		// is set on a per-request copy of the play settings; the user's persisted last-executed mode stays.
		ULevelEditorPlaySettings* FloatingSettings = DuplicateObject<ULevelEditorPlaySettings>(
			GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
		FloatingSettings->LastExecutedPlayModeType = PlayMode_InEditorFloating;
		RequestParams.EditorPlaySettings = FloatingSettings;
	}

	PIEState.HoldCPUThrottleBypass();
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

	if (!DeferredCallback)
	{
		return MissingCallbackError(TEXT("stop_pie"));
	}
	// ID unused: PIE lifecycle completes all general callbacks via broadcast
	const uint32 CallbackId = PIEState.RegisterPendingCallback(MoveTemp(DeferredCallback));
	(void)CallbackId;

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
	ECortexPIELaunchMode LaunchMode = ECortexPIELaunchMode::SelectedViewport;
	FCortexCommandResult ModeError;
	if (!TryReadLaunchMode(Params, LaunchMode, ModeError))
	{
		return ModeError;
	}

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
	if (!DeferredCallback)
	{
		return MissingCallbackError(TEXT("restart_pie"));
	}

	PIEState.SetState(ECortexPIEState::Stopping);
	GEditor->RequestEndPlayMap();

	const double RestartStartTime = FPlatformTime::Seconds();
	// Raw pointer: safe because PIEState is owned by FCortexEditorModule which outlives all tickers.
	// The 10s timeout provides an additional safety bound.
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
