#pragma once

#include "CoreMinimal.h"
#include "Session/CortexSessionTypes.h"
#include "Providers/CortexProviderTypes.h"

DECLARE_MULTICAST_DELEGATE(FOnCortexPendingChangesUpdated);

class FCortexFrontendSettings
{
public:
    static FCortexFrontendSettings& Get();

    ECortexAccessMode GetAccessMode() const { return AccessMode; }
    void SetAccessMode(ECortexAccessMode Mode);

    FString GetAccessModeString() const
    {
        switch (AccessMode)
        {
        case ECortexAccessMode::ReadOnly:   return TEXT("Read-Only");
        case ECortexAccessMode::Guided:     return TEXT("Guided");
        case ECortexAccessMode::FullAccess: return TEXT("Full Access");
        default:                            return TEXT("Unknown");
        }
    }

    bool GetSkipPermissions() const { return bSkipPermissions; }
    void SetSkipPermissions(bool bSkip);

    FString GetSelectedModel() const { return SelectedModel; }
    void SetSelectedModel(const FString& Model);
    TArray<FString> GetAvailableModels() const;
    TArray<FString> GetAvailableModelsForActiveProvider() const;

    ECortexEffortLevel GetEffortLevel() const { return EffortLevel; }
    void SetEffortLevel(ECortexEffortLevel Level);

    ECortexWorkflowMode GetWorkflowMode() const { return WorkflowMode; }
    void SetWorkflowMode(ECortexWorkflowMode Mode);

    bool GetProjectContext() const { return bProjectContext; }
    void SetProjectContext(bool bEnabled);

    bool GetAutoContext() const { return bAutoContext; }
    void SetAutoContext(bool bEnabled);

    FString GetCustomDirective() const { return CustomDirective; }
    void SetCustomDirective(const FString& Directive);

    FString GetEffortLevelString() const;

    static FString GetModelLabelWithEffort(const FString& ModelId)
    {
        const ECortexEffortLevel Level = Get().GetEffortLevel();
        if (Level == ECortexEffortLevel::Default)
        {
            return ModelId;
        }
        return FString::Printf(TEXT("%s [%s]"), *ModelId, *Get().GetEffortLevelString());
    }

    FCortexResolvedSessionOptions ResolveForActiveProvider() const;
    static FString FormatModelLabel(
        const FString& ProviderDisplayName,
        const FString& ModelId,
        ECortexEffortLevel EffortLevel,
        ECortexEffortLevel DefaultEffortLevel);

    bool HasPendingChanges() const;
    void ClearPendingChanges();

    FOnCortexPendingChangesUpdated OnPendingChangesUpdated;

    void Load();
    void Save();

private:
    FCortexFrontendSettings();
    FString GetSettingsFilePath() const;
    void MarkDirty();

    ECortexAccessMode AccessMode = ECortexAccessMode::ReadOnly;
    bool bSkipPermissions = true;
    FString SelectedModel = TEXT("Default");
    bool bHasCustomModels = false;
    TArray<FString> CustomModels;

    ECortexEffortLevel EffortLevel = ECortexEffortLevel::Default;
    ECortexWorkflowMode WorkflowMode = ECortexWorkflowMode::Direct;
    bool bProjectContext = true;
    bool bAutoContext = true;
    FString CustomDirective;
    bool bHasPendingChanges = false;
};
