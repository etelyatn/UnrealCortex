#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialModuleLoadTest,
	"Cortex.Material.ModuleLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialModuleLoadTest::RunTest(const FString& Parameters)
{
	FCortexMaterialCommandHandler Handler;

	// Verify handler returns UnknownCommand for an invalid command
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	FCortexCommandResult Result = Handler.Execute(TEXT("nonexistent_command"), Params);

	TestFalse(TEXT("Unknown command should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be UNKNOWN_COMMAND"),
		Result.ErrorCode, CortexErrorCodes::UnknownCommand);

	return true;
}
