#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "CortexFrontendSettings.h"
#include "CortexFrontendProviderSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendSettingsInvalidModelFallbackTest, "Cortex.Frontend.Settings.InvalidModelFallsBackToProviderDefault", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendSettingsInvalidEffortFallbackTest, "Cortex.Frontend.Settings.InvalidEffortFallsBackToProviderDefault", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFrontendSettingsInvalidModelFallbackTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    UCortexFrontendProviderSettings* ProviderSettings = GetMutableDefault<UCortexFrontendProviderSettings>();
    TestNotNull(TEXT("Provider settings should exist"), ProviderSettings);
    if (!ProviderSettings)
    {
        return false;
    }

    const FString OriginalProviderId = ProviderSettings->ActiveProviderId;
    const FString OriginalSelectedModel = Settings.GetSelectedModel();
    const ECortexEffortLevel OriginalEffortLevel = Settings.GetEffortLevel();
    ON_SCOPE_EXIT
    {
        ProviderSettings->ActiveProviderId = OriginalProviderId;
        Settings.SetSelectedModel(OriginalSelectedModel);
        Settings.SetEffortLevel(OriginalEffortLevel);
    };

    ProviderSettings->ActiveProviderId = TEXT("claude_code");
    Settings.SetSelectedModel(TEXT("claude-sonnet-4-6"));
    Settings.SetEffortLevel(ECortexEffortLevel::Maximum);

    const FCortexResolvedSessionOptions ClaudeResolved = Settings.ResolveForActiveProvider();
    TestEqual(TEXT("Claude selected model should be kept"), ClaudeResolved.ModelId, FString(TEXT("claude-sonnet-4-6")));
    TestEqual(TEXT("Claude effort should be kept"), static_cast<uint8>(ClaudeResolved.EffortLevel), static_cast<uint8>(ECortexEffortLevel::Maximum));

    ProviderSettings->ActiveProviderId = TEXT("codex");
    const FCortexResolvedSessionOptions CodexResolved = Settings.ResolveForActiveProvider();
    TestEqual(TEXT("Codex invalid Claude model should normalize to provider default"), CodexResolved.ModelId, FString(TEXT("gpt-5.4")));
    TestEqual(TEXT("Codex display name should stay Codex"), CodexResolved.ProviderDisplayName, FString(TEXT("Codex")));
    TestEqual(TEXT("Codex invalid Claude model should keep Maximum effort"), static_cast<uint8>(CodexResolved.EffortLevel), static_cast<uint8>(ECortexEffortLevel::Maximum));

    return true;
}

bool FCortexFrontendSettingsInvalidEffortFallbackTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    UCortexFrontendProviderSettings* ProviderSettings = GetMutableDefault<UCortexFrontendProviderSettings>();
    TestNotNull(TEXT("Provider settings should exist"), ProviderSettings);
    if (!ProviderSettings)
    {
        return false;
    }

    const FString OriginalProviderId = ProviderSettings->ActiveProviderId;
    const FString OriginalSelectedModel = Settings.GetSelectedModel();
    const ECortexEffortLevel OriginalEffortLevel = Settings.GetEffortLevel();
    ON_SCOPE_EXIT
    {
        ProviderSettings->ActiveProviderId = OriginalProviderId;
        Settings.SetSelectedModel(OriginalSelectedModel);
        Settings.SetEffortLevel(OriginalEffortLevel);
    };

    ProviderSettings->ActiveProviderId = TEXT("claude_code");
    Settings.SetSelectedModel(TEXT("claude-sonnet-4-6"));
    Settings.SetEffortLevel(ECortexEffortLevel::Maximum);

    const FCortexResolvedSessionOptions ClaudeResolved = Settings.ResolveForActiveProvider();
    TestEqual(TEXT("Claude keeps maximum effort"), static_cast<uint8>(ClaudeResolved.EffortLevel), static_cast<uint8>(ECortexEffortLevel::Maximum));

    ProviderSettings->ActiveProviderId = TEXT("codex");
    const FCortexResolvedSessionOptions CodexResolved = Settings.ResolveForActiveProvider();
    TestEqual(TEXT("Codex mapped max/xhigh should stay Maximum"), static_cast<uint8>(CodexResolved.EffortLevel), static_cast<uint8>(ECortexEffortLevel::Maximum));
    TestEqual(TEXT("Codex model should normalize to provider default"), CodexResolved.ModelId, FString(TEXT("gpt-5.4")));

    return true;
}
