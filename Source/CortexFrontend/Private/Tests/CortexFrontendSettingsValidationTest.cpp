#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "CortexFrontendSettings.h"
#include "CortexFrontendProviderSettings.h"
#include "Providers/CortexProviderRegistry.h"

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
    const FString SettingsFilePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexFrontend"), TEXT("settings.json"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(SettingsFilePath), true);
    const bool bHadOriginalSettingsFile = FPaths::FileExists(SettingsFilePath);
    FString OriginalSettingsJson;
    bool bCapturedOriginalSettings = !bHadOriginalSettingsFile || FFileHelper::LoadFileToString(OriginalSettingsJson, *SettingsFilePath);
    if (bHadOriginalSettingsFile && !bCapturedOriginalSettings)
    {
        AddInfo(TEXT("Skipping invalid-model fallback test because the existing settings file could not be captured safely."));
        return true;
    }
    ON_SCOPE_EXIT
    {
        if (bHadOriginalSettingsFile)
        {
            FFileHelper::SaveStringToFile(OriginalSettingsJson, *SettingsFilePath);
        }
        else
        {
            IFileManager::Get().Delete(*SettingsFilePath, false, true, true);
        }

        ProviderSettings->ActiveProviderId = OriginalProviderId;
        Settings.Load();
    };

    ProviderSettings->ActiveProviderId = TEXT("codex");
    Settings.SetSelectedModel(TEXT("claude-opus-4-6"));
    Settings.SetEffortLevel(ECortexEffortLevel::Low);

    const FCortexResolvedSessionOptions DirectCodexResolved = Settings.ResolveForActiveProvider();
    TestEqual(TEXT("Codex invalid Claude model should normalize to provider default"), DirectCodexResolved.ModelId, FString(TEXT("gpt-5.4")));
    TestEqual(TEXT("Codex display name should stay Codex"), DirectCodexResolved.ProviderDisplayName, FString(TEXT("Codex")));

    ProviderSettings->ActiveProviderId = TEXT("claude_code");
    Settings.SetSelectedModel(TEXT("claude-sonnet-4-6"));
    Settings.SetEffortLevel(ECortexEffortLevel::Maximum);

    const FCortexResolvedSessionOptions ClaudeResolved = Settings.ResolveForActiveProvider();
    TestEqual(TEXT("Claude selected model should be kept"), ClaudeResolved.ModelId, FString(TEXT("claude-sonnet-4-6")));
    TestEqual(TEXT("Claude effort should be kept"), static_cast<uint8>(ClaudeResolved.EffortLevel), static_cast<uint8>(ECortexEffortLevel::Maximum));

    ProviderSettings->ActiveProviderId = TEXT("codex");
    const FCortexResolvedSessionOptions CodexSwitchedResolved = Settings.ResolveForActiveProvider();
    TestEqual(TEXT("Codex invalid Claude model should normalize to provider default"), CodexSwitchedResolved.ModelId, FString(TEXT("gpt-5.4")));
    TestEqual(TEXT("Codex display name should stay Codex"), CodexSwitchedResolved.ProviderDisplayName, FString(TEXT("Codex")));
    TestEqual(TEXT("Codex invalid Claude model should keep Maximum effort"), static_cast<uint8>(CodexSwitchedResolved.EffortLevel), static_cast<uint8>(ECortexEffortLevel::Maximum));

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
    const FString SettingsFilePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexFrontend"), TEXT("settings.json"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(SettingsFilePath), true);
    const bool bHadOriginalSettingsFile = FPaths::FileExists(SettingsFilePath);
    FString OriginalSettingsJson;
    bool bCapturedOriginalSettings = !bHadOriginalSettingsFile || FFileHelper::LoadFileToString(OriginalSettingsJson, *SettingsFilePath);
    if (bHadOriginalSettingsFile && !bCapturedOriginalSettings)
    {
        AddInfo(TEXT("Skipping invalid-effort fallback test because the existing settings file could not be captured safely."));
        return true;
    }
    ON_SCOPE_EXIT
    {
        if (bHadOriginalSettingsFile)
        {
            FFileHelper::SaveStringToFile(OriginalSettingsJson, *SettingsFilePath);
        }
        else
        {
            IFileManager::Get().Delete(*SettingsFilePath, false, true, true);
        }

        ProviderSettings->ActiveProviderId = OriginalProviderId;
        Settings.Load();
    };

    ProviderSettings->ActiveProviderId = TEXT("codex");
    Settings.SetSelectedModel(TEXT("gpt-5.4"));
    Settings.SetEffortLevel(ECortexEffortLevel::Default);

    const FCortexResolvedSessionOptions CodexResolved = Settings.ResolveForActiveProvider();
    TestEqual(TEXT("Codex model should stay gpt-5.4"), CodexResolved.ModelId, FString(TEXT("gpt-5.4")));
    TestEqual(TEXT("Codex Default effort should fall back to Medium"), static_cast<uint8>(CodexResolved.EffortLevel), static_cast<uint8>(ECortexEffortLevel::Medium));

    ProviderSettings->ActiveProviderId = TEXT("claude_code");
    Settings.SetSelectedModel(TEXT("claude-sonnet-4-6"));
    Settings.SetEffortLevel(ECortexEffortLevel::Maximum);

    const FCortexResolvedSessionOptions ClaudeResolved = Settings.ResolveForActiveProvider();
    TestEqual(TEXT("Claude keeps maximum effort"), static_cast<uint8>(ClaudeResolved.EffortLevel), static_cast<uint8>(ECortexEffortLevel::Maximum));

    ProviderSettings->ActiveProviderId = TEXT("codex");
    Settings.SetEffortLevel(ECortexEffortLevel::Maximum);
    const FCortexResolvedSessionOptions SwitchedCodexResolved = Settings.ResolveForActiveProvider();
    TestEqual(TEXT("Codex mapped max/xhigh should stay Maximum"), static_cast<uint8>(SwitchedCodexResolved.EffortLevel), static_cast<uint8>(ECortexEffortLevel::Maximum));
    TestEqual(TEXT("Codex model should normalize to provider default"), SwitchedCodexResolved.ModelId, FString(TEXT("gpt-5.4")));

    return true;
}
