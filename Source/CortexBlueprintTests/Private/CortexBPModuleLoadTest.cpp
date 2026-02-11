#include "Misc/AutomationTest.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPModuleLoadTest,
	"Cortex.Blueprint.ModuleLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPModuleLoadTest::RunTest(const FString& Parameters)
{
	// Ensure CortexBlueprint module is loaded
	FModuleManager::Get().LoadModule(TEXT("CortexBlueprint"));

	// Verify CortexBlueprint module is loaded
	bool bIsLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("CortexBlueprint"));
	TestTrue(TEXT("CortexBlueprint module should be loaded"), bIsLoaded);

	// Verify bp domain is registered by executing get_capabilities
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	FCortexCommandResult CapResult = Router.Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_capabilities should succeed"), CapResult.bSuccess);

	// Verify "bp" domain appears in capabilities
	bool bFoundBP = false;
	if (CapResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* DomainsObj = nullptr;
		if (CapResult.Data->TryGetObjectField(TEXT("domains"), DomainsObj) && DomainsObj != nullptr)
		{
			bFoundBP = (*DomainsObj)->HasField(TEXT("bp"));
		}
		else
		{
			AddError(TEXT("get_capabilities did not return domains object"));
		}
	}
	else
	{
		AddError(TEXT("get_capabilities did not return valid data"));
	}

	TestTrue(TEXT("bp domain should be registered"), bFoundBP);

	return true;
}
