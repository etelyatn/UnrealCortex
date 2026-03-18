#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CortexGenTypes.h"
#include "CortexGenSettings.generated.h"

USTRUCT()
struct FCortexGenModelConfig
{
    GENERATED_BODY()

    UPROPERTY(Config, EditAnywhere, Category = "Model")
    FString ModelId;

    UPROPERTY(Config, EditAnywhere, Category = "Model")
    FString DisplayName;

    UPROPERTY(Config, EditAnywhere, Category = "Model")
    FString Provider;

    UPROPERTY(Config, EditAnywhere, Category = "Model")
    FString Category;   // "image" or "mesh"

    UPROPERTY(Config, EditAnywhere, Category = "Model",
        meta = (Bitmask, BitmaskEnum = "/Script/CortexGen.ECortexGenCapabilityFlags"))
    uint8 Capabilities = 0;

    UPROPERTY(Config, EditAnywhere, Category = "Model",
        meta = (ClampMin = 1, ClampMax = 4))
    int32 MaxBatchSize = 1;

    UPROPERTY(Config, EditAnywhere, Category = "Model")
    FString PricingNote;
};

UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig,
    meta = (DisplayName = "Cortex Gen", CategoryName = "Unreal Cortex"))
class CORTEXGEN_API UCortexGenSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    // Provider
    UPROPERTY(Config, EditAnywhere, Category = "Provider",
        meta = (GetOptions = "GetDefaultProviderOptions"))
    FString DefaultProvider = TEXT("meshy");

    UPROPERTY(Config, EditAnywhere, Category = "Provider|Meshy",
        meta = (PasswordField = true))
    FString MeshyApiKey;

    UPROPERTY(Config, EditAnywhere, Category = "Provider|Tripo3D",
        meta = (PasswordField = true))
    FString Tripo3DApiKey;

    UPROPERTY(Config, EditAnywhere, Category = "Provider|Tripo3D")
    FString Tripo3DModelVersion = TEXT("v2.0-20240919");

    UPROPERTY(Config, EditAnywhere, Category = "Provider|fal.ai",
        meta = (PasswordField = true))
    FString FalApiKey;

    UPROPERTY(Config, EditAnywhere, Category = "Provider|fal.ai")
    FString FalModelId = TEXT("fal-ai/hyper3d/rodin");

    UPROPERTY(Config, EditAnywhere, Category = "Provider|fal.ai",
        meta = (ToolTip = "fal.ai model for image generation (cheaper for prompt testing)"))
    FString FalImageModelId = TEXT("fal-ai/flux/dev");

    UPROPERTY(Config, EditAnywhere, Category = "Provider|fal.ai",
        meta = (GetOptions = "GetFalQualityOptions"))
    FString FalQuality = TEXT("medium");

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

    // Model Registry
    UPROPERTY(Config, EditAnywhere, Category = "Models",
        meta = (TitleProperty = "DisplayName"))
    TArray<FCortexGenModelConfig> ModelRegistry;

    UFUNCTION()
    static TArray<FString> GetDefaultProviderOptions()
    {
        return { TEXT("meshy"), TEXT("tripo3d"), TEXT("fal") };
    }

    UFUNCTION()
    static TArray<FString> GetFalQualityOptions()
    {
        return { TEXT("extra-low"), TEXT("low"), TEXT("medium"), TEXT("high") };
    }

    static const UCortexGenSettings* Get()
    {
        return GetDefault<UCortexGenSettings>();
    }

    // Helper: get API key for a provider ID (const — called via GetDefault<>())
    // Normalizes to lowercase to prevent case-mismatch failures
    FString GetApiKeyForProvider(const FString& ProviderId) const
    {
        FString Id = ProviderId.ToLower();
        if (Id == TEXT("fal")) return FalApiKey;
        if (Id == TEXT("meshy")) return MeshyApiKey;
        if (Id == TEXT("tripo3d")) return Tripo3DApiKey;
        return FString(); // Unknown provider — caller handles empty key
    }
};
