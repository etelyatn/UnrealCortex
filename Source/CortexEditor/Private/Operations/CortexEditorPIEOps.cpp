#include "Operations/CortexEditorPIEOps.h"
#include "CortexEditorPIEState.h"
#include "CortexCommandRouter.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "LevelEditor.h"

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
			TEXT("PIE_TRANSITION_IN_PROGRESS"),
			TEXT("PIE is currently starting/stopping. Wait and retry."));
	}
	if (PIEState.IsActive())
	{
		return FCortexCommandRouter::Error(
			TEXT("PIE_ALREADY_ACTIVE"),
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
			TEXT("PIE_TRANSITION_IN_PROGRESS"),
			TEXT("PIE is currently starting/stopping. Wait and retry."));
	}
	if (!PIEState.IsActive())
	{
		return FCortexCommandRouter::Error(
			TEXT("PIE_NOT_ACTIVE"),
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
