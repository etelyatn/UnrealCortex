#pragma once

#include "CoreMinimal.h"
#include "Session/CortexSessionTypes.h"

class FCortexFrontendSettings
{
public:
    static FCortexFrontendSettings& Get();

    ECortexAccessMode GetAccessMode() const { return AccessMode; }
    void SetAccessMode(ECortexAccessMode Mode);

    bool GetSkipPermissions() const { return bSkipPermissions; }
    void SetSkipPermissions(bool bSkip);

    void Load();
    void Save();

private:
    FCortexFrontendSettings();
    FString GetSettingsFilePath() const;

    ECortexAccessMode AccessMode = ECortexAccessMode::ReadOnly;
    bool bSkipPermissions = true;
};
