#include "Misc/AutomationTest.h"
#include "CortexFrontendSettings.h"
#include "Session/CortexCliSession.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexToolbarEffortBadgeDefaultTest,
    "Cortex.Frontend.ContextControls.Toolbar.EffortBadgeDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexToolbarEffortBadgeDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexEffortLevel Orig = Settings.GetEffortLevel();

    Settings.SetEffortLevel(ECortexEffortLevel::Default);

    const FString Label = FCortexFrontendSettings::GetModelLabelWithEffort(TEXT("claude-sonnet-4-6"));
    TestEqual(TEXT("No badge for default"), Label, TEXT("claude-sonnet-4-6"));

    Settings.SetEffortLevel(Orig);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexToolbarEffortBadgeMediumTest,
    "Cortex.Frontend.ContextControls.Toolbar.EffortBadgeMedium",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexToolbarEffortBadgeMediumTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexEffortLevel Orig = Settings.GetEffortLevel();

    Settings.SetEffortLevel(ECortexEffortLevel::Medium);

    const FString Label = FCortexFrontendSettings::GetModelLabelWithEffort(TEXT("claude-sonnet-4-6"));
    TestEqual(TEXT("Badge for medium"), Label, TEXT("claude-sonnet-4-6 [medium]"));

    Settings.SetEffortLevel(Orig);
    Settings.ClearPendingChanges();
    return true;
}
