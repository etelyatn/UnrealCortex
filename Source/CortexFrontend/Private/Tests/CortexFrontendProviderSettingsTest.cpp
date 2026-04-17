#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
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

    UCortexFrontendProviderSettings* Settings = GetMutableDefault<UCortexFrontendProviderSettings>();
    TestNotNull(TEXT("Settings mutable default should exist"), Settings);
    if (!Settings)
    {
        return false;
    }

    const FString OriginalProviderId = Settings->ActiveProviderId;
    ON_SCOPE_EXIT
    {
        Settings->ActiveProviderId = OriginalProviderId;
    };

    Settings->ActiveProviderId = TEXT("not-a-real-provider");
    TestEqual(TEXT("Effective provider helper should fall back to registry default"), Settings->GetEffectiveProviderId(), RegistryDefaultProviderId);
    TestEqual(TEXT("Effective provider definition should fall back to Claude"), Settings->GetEffectiveProviderDefinition().DisplayName, FString(TEXT("Claude Code")));

    Settings->ActiveProviderId = TEXT("codex");
    TestEqual(TEXT("Effective provider helper should resolve a valid provider directly"), Settings->GetEffectiveProviderId(), FString(TEXT("codex")));
    TestEqual(TEXT("Effective provider definition should resolve Codex"), Settings->GetEffectiveProviderDefinition().DisplayName, FString(TEXT("Codex")));

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
