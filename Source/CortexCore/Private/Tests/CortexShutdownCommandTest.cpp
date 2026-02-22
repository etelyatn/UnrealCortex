#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexShutdownCommandRegisteredTest,
	"Cortex.Core.Shutdown.CommandRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexShutdownCommandRegisteredTest::RunTest(const FString& Parameters)
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
			const TSharedPtr<FJsonObject>* CoreObj = nullptr;
			if ((*DomainsObj)->TryGetObjectField(TEXT("core"), CoreObj) && CoreObj != nullptr)
			{
				const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
				if ((*CoreObj)->TryGetArrayField(TEXT("commands"), Commands) && Commands != nullptr)
				{
					bool bFoundShutdown = false;
					for (const TSharedPtr<FJsonValue>& CmdVal : *Commands)
					{
						const TSharedPtr<FJsonObject>* CmdObj = nullptr;
						if (CmdVal->TryGetObject(CmdObj) && CmdObj != nullptr)
						{
							FString CmdName;
							if ((*CmdObj)->TryGetStringField(TEXT("name"), CmdName) && CmdName == TEXT("shutdown"))
							{
								bFoundShutdown = true;
								break;
							}
						}
					}
					TestTrue(TEXT("shutdown command found in core capabilities"), bFoundShutdown);
				}
			}
		}
	}

	return true;
}
