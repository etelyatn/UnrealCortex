#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreHandlerRegisteredTest,
	"Cortex.Core.Asset.HandlerRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCoreHandlerRegisteredTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	FCortexCommandResult CapResult = Router.Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_capabilities succeeded"), CapResult.bSuccess);

	if (CapResult.bSuccess && CapResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* DomainsObj = nullptr;
		if (CapResult.Data->TryGetObjectField(TEXT("domains"), DomainsObj) && DomainsObj != nullptr)
		{
			TestTrue(TEXT("core domain exists in capabilities"), (*DomainsObj)->HasField(TEXT("core")));
		}
		else
		{
			AddError(TEXT("domains field missing from capabilities"));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreUnknownCommandTest,
	"Cortex.Core.Asset.UnknownCommand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCoreUnknownCommandTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	FCortexCommandResult Result = Router.Execute(TEXT("core.nonexistent_command"), MakeShared<FJsonObject>());
	TestFalse(TEXT("Unknown command should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be UNKNOWN_COMMAND"), Result.ErrorCode, TEXT("UNKNOWN_COMMAND"));

	return true;
}
