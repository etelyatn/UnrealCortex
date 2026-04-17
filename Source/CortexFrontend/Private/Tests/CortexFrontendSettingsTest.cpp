#include "Misc/AutomationTest.h"
#include "CortexFrontendSettings.h"
#include "CortexFrontendProviderSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendSettingsDefaultTest, "Cortex.Frontend.Settings.DefaultIsReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendSettingsRoundTripTest, "Cortex.Frontend.Settings.RoundTripPersistence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendSettingsDeprecatedAvailableModelsTest, "Cortex.Frontend.Settings.DeprecatedAvailableModelsIgnored", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFrontendSettingsDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    TestEqual(TEXT("Default mode should be ReadOnly"), static_cast<uint8>(Settings.GetAccessMode()), static_cast<uint8>(ECortexAccessMode::ReadOnly));
    return true;
}

bool FCortexFrontendSettingsRoundTripTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexAccessMode OriginalMode = Settings.GetAccessMode();

    Settings.SetAccessMode(ECortexAccessMode::Guided);
    Settings.Load();
    TestEqual(TEXT("Guided mode should persist after reload"), static_cast<uint8>(Settings.GetAccessMode()), static_cast<uint8>(ECortexAccessMode::Guided));

    Settings.SetAccessMode(ECortexAccessMode::FullAccess);
    Settings.Load();
    TestEqual(TEXT("FullAccess mode should persist after reload"), static_cast<uint8>(Settings.GetAccessMode()), static_cast<uint8>(ECortexAccessMode::FullAccess));

    // Restore original
    Settings.SetAccessMode(OriginalMode);
    return true;
}

bool FCortexFrontendSettingsDeprecatedAvailableModelsTest::RunTest(const FString& Parameters)
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
    ON_SCOPE_EXIT
    {
        ProviderSettings->ActiveProviderId = OriginalProviderId;
    };

    ProviderSettings->ActiveProviderId = TEXT("codex");
    const TArray<FString> Models = Settings.GetAvailableModelsForActiveProvider();
    TestTrue(TEXT("Codex models should come from registry"), Models.Contains(TEXT("gpt-5.4")));
    TestFalse(TEXT("Codex models should not contain Claude-only models"), Models.Contains(TEXT("claude-opus-4-6")));

    return true;
}
