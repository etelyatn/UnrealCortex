#include "CortexFrontendSettings.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FCortexFrontendSettings& FCortexFrontendSettings::Get()
{
    static FCortexFrontendSettings Instance;
    return Instance;
}

FCortexFrontendSettings::FCortexFrontendSettings()
{
    Load();
}

void FCortexFrontendSettings::SetAccessMode(ECortexAccessMode Mode)
{
    AccessMode = Mode;
    Save();
}

void FCortexFrontendSettings::SetSkipPermissions(bool bSkip)
{
    bSkipPermissions = bSkip;
    Save();
}

void FCortexFrontendSettings::Load()
{
    const FString FilePath = GetSettingsFilePath();
    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
    {
        return;
    }

    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        return;
    }

    FString AccessModeString;
    if (JsonObject->TryGetStringField(TEXT("access_mode"), AccessModeString))
    {
        if (AccessModeString == TEXT("readonly"))
        {
            AccessMode = ECortexAccessMode::ReadOnly;
        }
        else if (AccessModeString == TEXT("guided"))
        {
            AccessMode = ECortexAccessMode::Guided;
        }
        else if (AccessModeString == TEXT("full"))
        {
            AccessMode = ECortexAccessMode::FullAccess;
        }
    }

    bool bStoredSkipPermissions = true;
    if (JsonObject->TryGetBoolField(TEXT("skip_permissions"), bStoredSkipPermissions))
    {
        bSkipPermissions = bStoredSkipPermissions;
    }
}

void FCortexFrontendSettings::Save()
{
    const FString FilePath = GetSettingsFilePath();
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);

    FString AccessModeString;
    switch (AccessMode)
    {
    case ECortexAccessMode::ReadOnly:
        AccessModeString = TEXT("readonly");
        break;
    case ECortexAccessMode::Guided:
        AccessModeString = TEXT("guided");
        break;
    case ECortexAccessMode::FullAccess:
        AccessModeString = TEXT("full");
        break;
    }

    TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
    JsonObject->SetStringField(TEXT("access_mode"), AccessModeString);
    JsonObject->SetBoolField(TEXT("skip_permissions"), bSkipPermissions);

    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
    Writer->Close();

    FFileHelper::SaveStringToFile(JsonString, *FilePath);
}

FString FCortexFrontendSettings::GetSettingsFilePath() const
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexFrontend"), TEXT("settings.json"));
}
