#include "Misc/AutomationTest.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGraphModuleLoadTest,
    "Cortex.Graph.ModuleLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphModuleLoadTest::RunTest(const FString& Parameters)
{
    // Ensure CortexGraph module is loaded
    FModuleManager::Get().LoadModule(TEXT("CortexGraph"));

    // Verify CortexGraph module is loaded
    bool bIsLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("CortexGraph"));
    TestTrue(TEXT("CortexGraph module should be loaded"), bIsLoaded);

    // Verify graph domain is registered by executing get_capabilities
    FCortexCoreModule& CoreModule =
        FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

    FCortexCommandResult CapResult = Router.Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
    TestTrue(TEXT("get_capabilities should succeed"), CapResult.bSuccess);

    // Verify "graph" domain appears in capabilities
    bool bFoundGraph = false;
    if (CapResult.Data.IsValid())
    {
        const TSharedPtr<FJsonObject>* DomainsObj = nullptr;
        if (CapResult.Data->TryGetObjectField(TEXT("domains"), DomainsObj) && DomainsObj != nullptr)
        {
            // domains is an object where keys are namespaces
            bFoundGraph = (*DomainsObj)->HasField(TEXT("graph"));
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

    TestTrue(TEXT("graph domain should be registered"), bFoundGraph);

    return true;
}
