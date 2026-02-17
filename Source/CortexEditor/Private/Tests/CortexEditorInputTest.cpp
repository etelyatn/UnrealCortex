#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInjectKeyNoPIETest,
	"Cortex.Editor.Input.InjectKey.ErrorWhenNoPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInjectKeyNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("key"), TEXT("W"));
	Params->SetStringField(TEXT("action"), TEXT("tap"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("inject_key"), Params);

	TestFalse(TEXT("inject_key should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error should be PIE_NOT_ACTIVE"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));

	return true;
}
