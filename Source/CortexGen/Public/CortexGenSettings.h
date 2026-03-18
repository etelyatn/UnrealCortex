#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CortexGenSettings.generated.h"

UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig,
    meta = (DisplayName = "Cortex Gen"))
class CORTEXGEN_API UCortexGenSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    // Provider
    UPROPERTY(Config, EditAnywhere, Category = "Provider")
    FString DefaultProvider = TEXT("meshy");

    UPROPERTY(Config, EditAnywhere, Category = "Provider|Meshy",
        meta = (PasswordField = true))
    FString MeshyApiKey;

    UPROPERTY(Config, EditAnywhere, Category = "Provider|Tripo3D",
        meta = (PasswordField = true))
    FString Tripo3DApiKey;

    // Import
    UPROPERTY(Config, EditAnywhere, Category = "Import")
    FString DefaultMeshDestination = TEXT("/Game/Generated/Meshes");

    UPROPERTY(Config, EditAnywhere, Category = "Import")
    FString DefaultTextureDestination = TEXT("/Game/Generated/Textures");

    // Polling
    UPROPERTY(Config, EditAnywhere, Category = "Advanced",
        meta = (ClampMin = 1, ClampMax = 60))
    int32 PollIntervalSeconds = 5;

    // Job Management
    UPROPERTY(Config, EditAnywhere, Category = "Advanced",
        meta = (ClampMin = 1, ClampMax = 10))
    int32 MaxConcurrentJobs = 2;

    UPROPERTY(Config, EditAnywhere, Category = "Advanced",
        meta = (ClampMin = 10, ClampMax = 500))
    int32 MaxJobHistory = 50;

    static const UCortexGenSettings* Get()
    {
        return GetDefault<UCortexGenSettings>();
    }
};
