#include "Misc/AutomationTest.h"
#include "CortexFrontendProviderSettings.h"
#include "Providers/CortexProviderRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendProviderSettingsDefaultProviderTest, "Cortex.Frontend.ProviderSettings.DefaultProviderIsClaude", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendProviderSettingsOptionsTest, "Cortex.Frontend.ProviderSettings.ProviderOptionsExposeClaudeAndCodex", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFrontendProviderSettingsDefaultProviderTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString RegistryDefaultProviderId = FCortexProviderRegistry::GetDefaultProviderId();
    const FCortexProviderDefinition* DefaultDefinition = FCortexProviderRegistry::FindDefinition(RegistryDefaultProviderId);

    TestEqual(TEXT("Settings class display name"), UCortexFrontendProviderSettings::StaticClass()->GetMetaData(TEXT("DisplayName")), FString(TEXT("Frontend")));
    TestEqual(TEXT("Settings helper default provider should come from registry"), UCortexFrontendProviderSettings::GetDefaultProviderId(), RegistryDefaultProviderId);
    TestNotNull(TEXT("Registry default provider should exist"), DefaultDefinition);
    if (!DefaultDefinition)
    {
        return false;
    }

    TestEqual(TEXT("Registry default provider should be Claude"), DefaultDefinition->ProviderId.ToString(), RegistryDefaultProviderId);

    UCortexFrontendProviderSettings* Settings = NewObject<UCortexFrontendProviderSettings>(GetTransientPackage());
    TestNotNull(TEXT("Transient settings object should exist"), Settings);
    if (!Settings)
    {
        return false;
    }

    TestEqual(TEXT("Transient settings object should use registry default provider"), Settings->ActiveProviderId, RegistryDefaultProviderId);
    TestTrue(TEXT("Help text should mention newly created sessions"), Settings->ProviderSelectionHelpText.Contains(TEXT("newly created")));
    TestTrue(TEXT("Help text should mention current sessions do not restart"), Settings->ProviderSelectionHelpText.Contains(TEXT("current sessions do not restart")));

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
