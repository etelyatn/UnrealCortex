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
        // If settings unavailable, IsEnabled returns false (opt-in default — no CDO means disabled)
        TestFalse(TEXT("IsEnabled should be false when settings unavailable"), bResult);
    }

    return true;
}
