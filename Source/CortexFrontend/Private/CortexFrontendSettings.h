#pragma once

#include "CoreMinimal.h"
#include "Session/CortexSessionTypes.h"

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

    void Load();
    void Save();

private:
    FCortexFrontendSettings();
    FString GetSettingsFilePath() const;

    ECortexAccessMode AccessMode = ECortexAccessMode::ReadOnly;
    bool bSkipPermissions = true;
    FString SelectedModel = TEXT("Default");
    bool bHasCustomModels = false;
    TArray<FString> CustomModels;
};
