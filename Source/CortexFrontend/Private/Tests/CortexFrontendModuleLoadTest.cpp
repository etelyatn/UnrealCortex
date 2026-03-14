#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "Framework/Docking/TabManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexFrontendModuleLoadTest,
    "Cortex.Frontend.ModuleLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFrontendModuleLoadTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FModuleManager::Get().LoadModule(TEXT("CortexFrontend"));

    const bool bIsLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("CortexFrontend"));
    TestTrue(TEXT("CortexFrontend module should be loaded"), bIsLoaded);

    // Verify the tab is registered under the new "CortexFrontend" ID (not the old "CortexChat")
    const bool bHasSpawner = FGlobalTabmanager::Get()->HasTabSpawner(FName(TEXT("CortexFrontend")));
    TestTrue(TEXT("CortexFrontend tab ID should be registered"), bHasSpawner);

    return true;
}
