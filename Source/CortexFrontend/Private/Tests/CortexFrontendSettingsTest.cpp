#include "Misc/AutomationTest.h"
#include "CortexFrontendSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendSettingsDefaultTest, "Cortex.Frontend.Settings.DefaultIsReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFrontendSettingsDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    TestEqual(TEXT("Default mode should be ReadOnly"), static_cast<uint8>(Settings.GetAccessMode()), static_cast<uint8>(ECortexAccessMode::ReadOnly));
    return true;
}
