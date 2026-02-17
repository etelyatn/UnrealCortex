#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorGetPIEStateTest,
	"Cortex.Editor.PIE.GetPIEState.WhenStopped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorGetPIEStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_pie_state"), Params);
	TestTrue(TEXT("get_pie_state should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString State;
		Result.Data->TryGetStringField(TEXT("state"), State);
		TestEqual(TEXT("State should be 'stopped'"), State, TEXT("stopped"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorGetEditorStateTest,
	"Cortex.Editor.Utility.GetEditorState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorGetEditorStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_editor_state"), Params);
	TestTrue(TEXT("get_editor_state should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString PIEState;
		Result.Data->TryGetStringField(TEXT("pie_state"), PIEState);
		TestEqual(TEXT("PIE state should be 'stopped'"), PIEState, TEXT("stopped"));

		FString ProjectName;
		Result.Data->TryGetStringField(TEXT("project_name"), ProjectName);
		TestFalse(TEXT("Project name should not be empty"), ProjectName.IsEmpty());

		TestTrue(TEXT("Should have current_map"), Result.Data->HasField(TEXT("current_map")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorStartPIENotActiveTest,
	"Cortex.Editor.PIE.StartPIE.ReturnsDeferred",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorStartPIENotActiveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("mode"), TEXT("selected_viewport"));

	bool bCallbackFired = false;
	FDeferredResponseCallback Callback = [&bCallbackFired](FCortexCommandResult Result)
	{
		(void)Result;
		bCallbackFired = true;
	};

	const FCortexCommandResult Result = Handler.Execute(TEXT("start_pie"), Params, MoveTemp(Callback));
	TestTrue(TEXT("start_pie should be deferred"), Result.bIsDeferred);

	return true;
}
