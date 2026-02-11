
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexModuleLoadTest,
	"Cortex.Core.Module.PluginLoads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexModuleLoadTest::RunTest(const FString& Parameters)
{
	const bool bLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore"));
	TestTrue(TEXT("CortexCore module should be loaded"), bLoaded);
	return true;
}
