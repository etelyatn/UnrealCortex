#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

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

    return true;
}
