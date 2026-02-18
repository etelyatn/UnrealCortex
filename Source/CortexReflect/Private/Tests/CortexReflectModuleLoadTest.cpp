#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectModuleLoadTest,
	"Cortex.Reflect.ModuleLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectModuleLoadTest::RunTest(const FString& Parameters)
{
	const bool bLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("CortexReflect"));
	TestTrue(TEXT("CortexReflect module should be loaded"), bLoaded);
	return true;
}
