#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CortexFrontendProviderSettings.generated.h"

UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig, meta = (DisplayName = "Frontend", CategoryName = "Unreal Cortex"))
class CORTEXFRONTEND_API UCortexFrontendProviderSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UCortexFrontendProviderSettings();

    UPROPERTY(Config, EditAnywhere, Category = "Provider", meta = (GetOptions = "GetProviderOptions"))
    FString ActiveProviderId;

    UPROPERTY(VisibleAnywhere, Category = "Provider", meta = (MultiLine = true))
    FString ProviderSelectionHelpText;

    UFUNCTION()
    static TArray<FString> GetProviderOptions();

    static FString GetDefaultProviderId();
    FString GetEffectiveProviderId() const;

    static const UCortexFrontendProviderSettings* Get();
};
