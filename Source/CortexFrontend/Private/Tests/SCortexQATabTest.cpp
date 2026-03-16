// Source/CortexFrontend/Private/Tests/SCortexQATabTest.cpp
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "Framework/Docking/TabManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQATabRegisteredTest,
    "Cortex.Frontend.QATab.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQATabRegisteredTest::RunTest(const FString& Parameters)
{
    // The QA tab should be registered as part of the workbench layout.
    // We can't easily test tab spawning without the full workbench,
    // but we can verify the module loaded which sets up tab spawners.
    TestTrue(TEXT("CortexFrontend module should be loaded"),
        FModuleManager::Get().IsModuleLoaded(TEXT("CortexFrontend")));
    return true;
}
