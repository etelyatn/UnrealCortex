#include "Operations/CortexEditorInputOps.h"
#include "CortexEditorPIEState.h"
#include "CortexCommandRouter.h"
#include "Framework/Application/SlateApplication.h"

namespace
{
FCortexCommandResult ValidateInputContext(const FCortexEditorPIEState& PIEState)
{
	if (!PIEState.IsActive())
	{
		return FCortexCommandRouter::Error(
			TEXT("PIE_NOT_ACTIVE"),
			TEXT("PIE is not running. Call start_pie first."));
	}

	if (!FSlateApplication::IsInitialized())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Slate application not initialized"));
	}

	return FCortexCommandRouter::Success(nullptr);
}
}

FCortexCommandResult FCortexEditorInputOps::InjectKey(
	const FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	const FCortexCommandResult Context = ValidateInputContext(PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	(void)Params;
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("accepted"));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorInputOps::InjectMouse(
	const FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	const FCortexCommandResult Context = ValidateInputContext(PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	(void)Params;
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("accepted"));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorInputOps::InjectInputAction(
	const FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	const FCortexCommandResult Context = ValidateInputContext(PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	(void)Params;
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("accepted"));
	return FCortexCommandRouter::Success(Data);
}
