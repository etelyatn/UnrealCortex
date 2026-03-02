#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorFocusNodeMissingParamsTest,
	"Cortex.Editor.FocusNode.MissingParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorFocusNodeMissingParamsTest::RunTest(const FString& Parameters)
{
	FCortexEditorCommandHandler Handler;

	TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
	FCortexCommandResult Result = Handler.Execute(TEXT("focus_node"), EmptyParams);
	TestFalse(TEXT("focus_node without params should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> NoNodeParams = MakeShared<FJsonObject>();
	NoNodeParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Test/BP_Test"));
	FCortexCommandResult Result2 = Handler.Execute(TEXT("focus_node"), NoNodeParams);
	TestFalse(TEXT("focus_node without node_id should fail"), Result2.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result2.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorFocusNodeAssetNotFoundTest,
	"Cortex.Editor.FocusNode.AssetNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorFocusNodeAssetNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexEditorCommandHandler Handler;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/BP_Nothing"));
	Params->SetStringField(TEXT("node_id"), TEXT("K2Node_Event_0"));
	FCortexCommandResult Result = Handler.Execute(TEXT("focus_node"), Params);

	TestFalse(TEXT("focus_node with non-existent asset should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::AssetNotFound);

	return true;
}
