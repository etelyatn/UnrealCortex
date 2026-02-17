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
