#include "Misc/AutomationTest.h"
#include "CortexGenModule.h"
#include "CortexGenSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenModuleIsEnabledTest,
    "Cortex.Gen.Module.IsEnabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenModuleIsEnabledTest::RunTest(const FString& Parameters)
{
    // IsEnabled() should match the bEnabled setting value
    const bool bResult = FCortexGenModule::IsEnabled();
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();

    if (Settings)
    {
        TestEqual(TEXT("IsEnabled should match settings bEnabled"),
            bResult, Settings->bEnabled);
    }
    else
    {
        // If settings unavailable, IsEnabled should default to true (fail-safe)
        TestTrue(TEXT("IsEnabled should be true when settings unavailable"), bResult);
    }

    return true;
}
