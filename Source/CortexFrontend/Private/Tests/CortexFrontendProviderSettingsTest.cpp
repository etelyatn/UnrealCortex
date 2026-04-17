#include "Misc/AutomationTest.h"
#include "CortexFrontendProviderSettings.h"
#include "Providers/CortexProviderRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendProviderSettingsDefaultProviderTest, "Cortex.Frontend.ProviderSettings.DefaultProviderIsClaude", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendProviderSettingsOptionsTest, "Cortex.Frontend.ProviderSettings.ProviderOptionsExposeClaudeAndCodex", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFrontendProviderSettingsDefaultProviderTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    TestEqual(TEXT("Settings class display name"), UCortexFrontendProviderSettings::StaticClass()->GetMetaData(TEXT("DisplayName")), FString(TEXT("Frontend")));
    const FString RegistryDefaultProviderId = FCortexProviderRegistry::GetDefaultProviderId();
    TestEqual(TEXT("Registry default provider id"), RegistryDefaultProviderId, FString(TEXT("claude_code")));

    const FCortexProviderDefinition& DefaultDefinition = FCortexProviderRegistry::GetDefaultDefinition();
    TestEqual(TEXT("Registry default provider should be Claude"), DefaultDefinition.DisplayName, FString(TEXT("Claude Code")));
    TestEqual(TEXT("Settings helper default provider should come from registry"), UCortexFrontendProviderSettings::GetDefaultProviderId(), RegistryDefaultProviderId);
    TestEqual(TEXT("Registry resolves the explicit default id"), FCortexProviderRegistry::ResolveDefinition(RegistryDefaultProviderId).DisplayName, FString(TEXT("Claude Code")));
    TestEqual(TEXT("Registry resolves unknown ids to the default"), FCortexProviderRegistry::ResolveDefinition(TEXT("not-a-real-provider")).ProviderId.ToString(), RegistryDefaultProviderId);

    const UCortexFrontendProviderSettings* SettingsCDO = GetDefault<UCortexFrontendProviderSettings>();
    TestNotNull(TEXT("Settings CDO should exist"), SettingsCDO);
    if (!SettingsCDO)
    {
        return false;
    }

    TestTrue(TEXT("Help text should mention newly created sessions"), SettingsCDO->ProviderSelectionHelpText.Contains(TEXT("newly created")));
    TestTrue(TEXT("Help text should mention current sessions do not restart"), SettingsCDO->ProviderSelectionHelpText.Contains(TEXT("current sessions do not restart")));

    return true;
}

bool FCortexFrontendProviderSettingsOptionsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TArray<FString> Options = UCortexFrontendProviderSettings::GetProviderOptions();
    const TArray<FString> RegistryOptions = FCortexProviderRegistry::GetProviderOptions();
    TestEqual(TEXT("Provider options should come from registry"), Options, RegistryOptions);
    TestTrue(TEXT("Provider options should include Claude"), Options.Contains(TEXT("claude_code")));
    TestTrue(TEXT("Provider options should include Codex"), Options.Contains(TEXT("codex")));

    return true;
}
