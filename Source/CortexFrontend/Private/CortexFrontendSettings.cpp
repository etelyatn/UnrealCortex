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
    check(IsInGameThread());
    if (AccessMode == Mode) return;
    AccessMode = Mode;
    MarkDirty();
    Save();
}

void FCortexFrontendSettings::SetSkipPermissions(bool bSkip)
{
    check(IsInGameThread());
    if (bSkipPermissions == bSkip) return;
    bSkipPermissions = bSkip;
    MarkDirty();
    Save();
}

void FCortexFrontendSettings::SetSelectedModel(const FString& Model)
{
    check(IsInGameThread());
    if (SelectedModel == Model) return;
    SelectedModel = Model;
    MarkDirty();
    Save();
}

TArray<FString> FCortexFrontendSettings::GetAvailableModels() const
{
    if (bHasCustomModels)
    {
        return CustomModels;
    }
    // Default list — updating the plugin binary updates these
    return {TEXT("Default"), TEXT("claude-sonnet-4-6"), TEXT("claude-opus-4-6"), TEXT("claude-haiku-4-5-20251001")};
}

void FCortexFrontendSettings::SetEffortLevel(ECortexEffortLevel Level)
{
    check(IsInGameThread());
    if (EffortLevel == Level) return;
    EffortLevel = Level;
    MarkDirty();
    Save();
}

void FCortexFrontendSettings::SetWorkflowMode(ECortexWorkflowMode Mode)
{
    check(IsInGameThread());
    if (WorkflowMode == Mode) return;
    WorkflowMode = Mode;
    MarkDirty();
    Save();
}

void FCortexFrontendSettings::SetProjectContext(bool bEnabled)
{
    check(IsInGameThread());
    if (bProjectContext == bEnabled) return;
    bProjectContext = bEnabled;
    MarkDirty();
    Save();
}

void FCortexFrontendSettings::SetCustomDirective(const FString& Directive)
{
    check(IsInGameThread());
    FString Clamped = Directive.Left(500);
    if (CustomDirective == Clamped) return;
    CustomDirective = Clamped;
    MarkDirty();
    Save();
}

bool FCortexFrontendSettings::HasPendingChanges() const
{
    check(IsInGameThread());
    return bHasPendingChanges;
}

void FCortexFrontendSettings::ClearPendingChanges()
{
    check(IsInGameThread());
    if (bHasPendingChanges)
    {
        bHasPendingChanges = false;
        OnPendingChangesUpdated.Broadcast();
    }
}

void FCortexFrontendSettings::MarkDirty()
{
    check(IsInGameThread());
    bHasPendingChanges = true;
    OnPendingChangesUpdated.Broadcast();
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

    if (JsonObject->TryGetStringField(TEXT("selected_model"), SelectedModel) == false)
    {
        SelectedModel = TEXT("Default");
    }

    const TArray<TSharedPtr<FJsonValue>>* ModelsArray = nullptr;
    if (JsonObject->TryGetArrayField(TEXT("available_models"), ModelsArray))
    {
        CustomModels.Reset();
        for (const TSharedPtr<FJsonValue>& Value : *ModelsArray)
        {
            FString ModelStr;
            if (Value->TryGetString(ModelStr))
            {
                CustomModels.Add(ModelStr);
            }
        }
        // Only use custom list if it's non-empty; empty array falls back to defaults
        bHasCustomModels = CustomModels.Num() > 0;
    }

    FString EffortString;
    if (JsonObject->TryGetStringField(TEXT("effort_level"), EffortString))
    {
        if (EffortString == TEXT("low"))
        {
            EffortLevel = ECortexEffortLevel::Low;
        }
        else if (EffortString == TEXT("medium"))
        {
            EffortLevel = ECortexEffortLevel::Medium;
        }
        else if (EffortString == TEXT("high"))
        {
            EffortLevel = ECortexEffortLevel::High;
        }
        else if (EffortString == TEXT("max"))
        {
            EffortLevel = ECortexEffortLevel::Maximum;
        }
        else
        {
            EffortLevel = ECortexEffortLevel::Default;
        }
    }

    FString WorkflowString;
    if (JsonObject->TryGetStringField(TEXT("workflow_mode"), WorkflowString))
    {
        WorkflowMode = (WorkflowString == TEXT("thorough"))
            ? ECortexWorkflowMode::Thorough
            : ECortexWorkflowMode::Direct;
    }

    bool bStoredProjectContext = true;
    if (JsonObject->TryGetBoolField(TEXT("project_context"), bStoredProjectContext))
    {
        bProjectContext = bStoredProjectContext;
    }

    FString StoredDirective;
    if (JsonObject->TryGetStringField(TEXT("custom_directive"), StoredDirective))
    {
        CustomDirective = StoredDirective;
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
    JsonObject->SetStringField(TEXT("selected_model"), SelectedModel);
    JsonObject->SetStringField(TEXT("effort_level"), GetEffortLevelString());
    JsonObject->SetStringField(TEXT("workflow_mode"),
        WorkflowMode == ECortexWorkflowMode::Thorough ? TEXT("thorough") : TEXT("direct"));
    JsonObject->SetBoolField(TEXT("project_context"), bProjectContext);
    JsonObject->SetStringField(TEXT("custom_directive"), CustomDirective);

    if (bHasCustomModels)
    {
        TArray<TSharedPtr<FJsonValue>> ModelsArray;
        for (const FString& Model : CustomModels)
        {
            ModelsArray.Add(MakeShareable(new FJsonValueString(Model)));
        }
        JsonObject->SetArrayField(TEXT("available_models"), ModelsArray);
    }

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
