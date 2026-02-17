#include "Misc/AutomationTest.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorErrorCodesTest,
	"Cortex.Core.ErrorCodes.EditorDomain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorErrorCodesTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("PIE_NOT_ACTIVE code should match"), CortexErrorCodes::PIENotActive, TEXT("PIE_NOT_ACTIVE"));
	TestEqual(TEXT("PIE_ALREADY_ACTIVE code should match"), CortexErrorCodes::PIEAlreadyActive, TEXT("PIE_ALREADY_ACTIVE"));
	TestEqual(TEXT("PIE_ALREADY_PAUSED code should match"), CortexErrorCodes::PIEAlreadyPaused, TEXT("PIE_ALREADY_PAUSED"));
	TestEqual(TEXT("PIE_NOT_PAUSED code should match"), CortexErrorCodes::PIENotPaused, TEXT("PIE_NOT_PAUSED"));
	TestEqual(TEXT("PIE_TRANSITION_IN_PROGRESS code should match"), CortexErrorCodes::PIETransitionInProgress, TEXT("PIE_TRANSITION_IN_PROGRESS"));
	TestEqual(TEXT("PIE_TERMINATED code should match"), CortexErrorCodes::PIETerminated, TEXT("PIE_TERMINATED"));
	TestEqual(TEXT("PIE_MODE_UNSUPPORTED code should match"), CortexErrorCodes::PIEModeUnsupported, TEXT("PIE_MODE_UNSUPPORTED"));
	TestEqual(TEXT("VIEWPORT_NOT_FOUND code should match"), CortexErrorCodes::ViewportNotFound, TEXT("VIEWPORT_NOT_FOUND"));
	TestEqual(TEXT("INPUT_ACTION_NOT_FOUND code should match"), CortexErrorCodes::InputActionNotFound, TEXT("INPUT_ACTION_NOT_FOUND"));
	TestEqual(TEXT("SCREENSHOT_FAILED code should match"), CortexErrorCodes::ScreenshotFailed, TEXT("SCREENSHOT_FAILED"));
	TestEqual(TEXT("CONSOLE_COMMAND_FAILED code should match"), CortexErrorCodes::ConsoleCommandFailed, TEXT("CONSOLE_COMMAND_FAILED"));
	TestEqual(TEXT("INVALID_TIME_SCALE code should match"), CortexErrorCodes::InvalidTimeScale, TEXT("INVALID_TIME_SCALE"));
	TestEqual(TEXT("GAME_MODE_NOT_FOUND code should match"), CortexErrorCodes::GameModeNotFound, TEXT("GAME_MODE_NOT_FOUND"));
	return true;
}
