#include "Misc/AutomationTest.h"
#include "CortexFrontendSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexToolbarEffortBadgeDefaultTest,
    "Cortex.Frontend.ContextControls.Toolbar.EffortBadgeDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexToolbarEffortBadgeDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString Label = FCortexFrontendSettings::FormatModelLabel(
        TEXT("Claude Code"),
        TEXT("claude-sonnet-4-6"),
        ECortexEffortLevel::Default,
        ECortexEffortLevel::Default);
    TestEqual(TEXT("Provider-aware default label omits effort badge"),
        Label,
        FString(TEXT("Claude Code · claude-sonnet-4-6")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexToolbarEffortBadgeMediumTest,
    "Cortex.Frontend.ContextControls.Toolbar.EffortBadgeMedium",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexToolbarEffortBadgeMediumTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString Label = FCortexFrontendSettings::FormatModelLabel(
        TEXT("Codex"),
        TEXT("gpt-5.4"),
        ECortexEffortLevel::Maximum,
        ECortexEffortLevel::Medium);
    TestEqual(TEXT("Provider-aware label includes non-default effort badge"),
        Label,
        FString(TEXT("Codex · gpt-5.4 [max]")));
    return true;
}
