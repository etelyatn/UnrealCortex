#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "CortexFrontendSettings.h"
#include "CortexFrontendProviderSettings.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    FString MakeIsolatedMigrationSettingsFilePath(const TCHAR* TestName)
    {
        return FPaths::Combine(
            FPaths::ProjectSavedDir(),
            TEXT("CortexFrontend"),
            TEXT("Test"),
            FString::Printf(TEXT("%s-%s.json"), TestName, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendSettingsDefaultTest, "Cortex.Frontend.Settings.DefaultIsReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendSettingsRoundTripTest, "Cortex.Frontend.Settings.RoundTripPersistence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendSettingsDeprecatedAvailableModelsTest, "Cortex.Frontend.Settings.DeprecatedAvailableModelsIgnored", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFrontendSettingsDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString SettingsFilePath = MakeIsolatedMigrationSettingsFilePath(TEXT("DefaultIsReadOnly"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(SettingsFilePath), true);
    FCortexFrontendSettings::SetSettingsFilePathOverrideForTests(SettingsFilePath);
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    Settings.Load();
    ON_SCOPE_EXIT
    {
        FCortexFrontendSettings::ClearSettingsFilePathOverrideForTests();
        IFileManager::Get().Delete(*SettingsFilePath, false, true, true);
        Settings.Load();
    };
    TestEqual(TEXT("Default mode should be ReadOnly"), static_cast<uint8>(Settings.GetAccessMode()), static_cast<uint8>(ECortexAccessMode::ReadOnly));
    return true;
}

bool FCortexFrontendSettingsRoundTripTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString SettingsFilePath = MakeIsolatedMigrationSettingsFilePath(TEXT("RoundTripPersistence"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(SettingsFilePath), true);
    FCortexFrontendSettings::SetSettingsFilePathOverrideForTests(SettingsFilePath);
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    Settings.Load();
    const ECortexAccessMode OriginalMode = Settings.GetAccessMode();
    ON_SCOPE_EXIT
    {
        FCortexFrontendSettings::ClearSettingsFilePathOverrideForTests();
        IFileManager::Get().Delete(*SettingsFilePath, false, true, true);
        Settings.Load();
    };

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

    UCortexFrontendProviderSettings* ProviderSettings = GetMutableDefault<UCortexFrontendProviderSettings>();
    TestNotNull(TEXT("Provider settings should exist"), ProviderSettings);
    if (!ProviderSettings)
    {
        return false;
    }

    const FString OriginalProviderId = ProviderSettings->ActiveProviderId;
    const FString SettingsFilePath = MakeIsolatedMigrationSettingsFilePath(TEXT("DeprecatedAvailableModels"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(SettingsFilePath), true);
    FCortexFrontendSettings::SetSettingsFilePathOverrideForTests(SettingsFilePath);
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    Settings.Load();
    ON_SCOPE_EXIT
    {
        ProviderSettings->ActiveProviderId = OriginalProviderId;
        FCortexFrontendSettings::ClearSettingsFilePathOverrideForTests();
        Settings.Load();
        IFileManager::Get().Delete(*SettingsFilePath, false, true, true);
    };

    TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
    JsonObject->SetStringField(TEXT("access_mode"), TEXT("readonly"));
    JsonObject->SetBoolField(TEXT("skip_permissions"), true);
    JsonObject->SetStringField(TEXT("selected_model"), TEXT("legacy-fake-model"));
    JsonObject->SetStringField(TEXT("effort_level"), TEXT("default"));
    JsonObject->SetStringField(TEXT("workflow_mode"), TEXT("direct"));
    JsonObject->SetBoolField(TEXT("project_context"), true);
    JsonObject->SetBoolField(TEXT("auto_context"), true);
    JsonObject->SetStringField(TEXT("custom_directive"), TEXT(""));

    TArray<TSharedPtr<FJsonValue>> ModelsArray;
    ModelsArray.Add(MakeShared<FJsonValueString>(TEXT("legacy-fake-model")));
    ModelsArray.Add(MakeShared<FJsonValueString>(TEXT("legacy-secondary-model")));
    JsonObject->SetArrayField(TEXT("available_models"), ModelsArray);

    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    TestTrue(TEXT("Should serialize temp settings payload"), FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer));
    Writer->Close();
    TestTrue(TEXT("Should write temp settings payload"), FFileHelper::SaveStringToFile(JsonString, *SettingsFilePath));

    ProviderSettings->ActiveProviderId = TEXT("codex");
    Settings.Load();
    const TArray<FString> Models = Settings.GetAvailableModelsForActiveProvider();
    TestTrue(TEXT("Codex models should come from registry"), Models.Contains(TEXT("gpt-5.4")));
    TestFalse(TEXT("Codex models should not contain Claude-only models"), Models.Contains(TEXT("claude-opus-4-6")));
    TestFalse(TEXT("Codex models should ignore deprecated JSON payload"), Models.Contains(TEXT("legacy-fake-model")));
    TestTrue(TEXT("Deprecated JSON model list should still load for compatibility"), Settings.GetAvailableModels().Contains(TEXT("legacy-fake-model")));

    Settings.Save();
    FString SavedJson;
    TestTrue(TEXT("Should reload saved settings file"), FFileHelper::LoadFileToString(SavedJson, *SettingsFilePath));
    TSharedPtr<FJsonObject> SavedJsonObject;
    TSharedRef<TJsonReader<>> SavedReader = TJsonReaderFactory<>::Create(SavedJson);
    TestTrue(TEXT("Should parse saved settings file"), FJsonSerializer::Deserialize(SavedReader, SavedJsonObject));
    TestNotNull(TEXT("Saved settings JSON should be valid"), SavedJsonObject.Get());
    if (SavedJsonObject.IsValid())
    {
        TestFalse(TEXT("Deprecated available_models field should not be persisted"), SavedJsonObject->HasField(TEXT("available_models")));
    }
    TestFalse(TEXT("Deprecated JSON model list should be cleared from memory after save"), Settings.GetAvailableModels().Contains(TEXT("legacy-fake-model")));

    return true;
}
