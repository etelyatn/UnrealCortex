#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorGetViewportInfoTest,
	"Cortex.Editor.Viewport.GetViewportInfo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorGetViewportInfoTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_viewport_info"), Params);
	TestTrue(TEXT("get_viewport_info should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("Should have resolution"), Result.Data->HasField(TEXT("resolution")));
		TestTrue(TEXT("Should have camera_location"), Result.Data->HasField(TEXT("camera_location")));
		TestTrue(TEXT("Should have view_mode"), Result.Data->HasField(TEXT("view_mode")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorSetViewportModeInvalidTest,
	"Cortex.Editor.Viewport.SetViewportMode.InvalidMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorSetViewportModeInvalidTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("mode"), TEXT("not_a_mode"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_viewport_mode"), Params);
	TestFalse(TEXT("set_viewport_mode should fail for invalid mode"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_VALUE"), Result.ErrorCode, TEXT("INVALID_VALUE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorSetViewportCameraMissingLocationTest,
	"Cortex.Editor.Viewport.SetViewportCamera.MissingLocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorSetViewportCameraMissingLocationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_viewport_camera"), Params);
	TestFalse(TEXT("set_viewport_camera should fail when location missing"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, TEXT("INVALID_FIELD"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorFocusActorMissingPathTest,
	"Cortex.Editor.Viewport.FocusActor.MissingActorPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorFocusActorMissingPathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("focus_actor"), Params);
	TestFalse(TEXT("focus_actor should fail when actor_path missing"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, TEXT("INVALID_FIELD"));
	return true;
}
