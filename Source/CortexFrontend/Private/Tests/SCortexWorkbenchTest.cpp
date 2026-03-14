#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "Framework/Docking/TabManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchModuleTest,
	"Cortex.Frontend.Workbench.ModuleLoaded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexWorkbenchModuleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestTrue(TEXT("CortexFrontend module should be loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("CortexFrontend")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchTabIdTest,
	"Cortex.Frontend.Workbench.TabId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexWorkbenchTabIdTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Verify the tab ID is "CortexFrontend" (not the old "CortexChat")
	const FName ExpectedTabId(TEXT("CortexFrontend"));
	const bool bHasSpawner = FGlobalTabmanager::Get()->HasTabSpawner(ExpectedTabId);
	TestTrue(TEXT("CortexFrontend tab should be registered"), bHasSpawner);
	return true;
}
