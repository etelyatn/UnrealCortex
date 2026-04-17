#pragma once

#include "CoreMinimal.h"
#include "CortexProviderTypes.h"

class FCortexProviderRegistry
{
public:
    static const FCortexProviderDefinition* FindDefinition(const FString& ProviderId);
    static const FCortexProviderDefinition& GetDefaultDefinition();
    static const FCortexProviderDefinition& ResolveDefinition(const FString& ProviderId);
    static const FCortexProviderModelDefinition& ValidateOrGetDefaultModel(
        const FCortexProviderDefinition& ProviderDefinition,
        const FString& ModelId);
    static ECortexEffortLevel ValidateOrGetDefaultEffort(
        const FCortexProviderDefinition& ProviderDefinition,
        const FCortexProviderModelDefinition& ModelDefinition,
        ECortexEffortLevel EffortLevel);
    static int64 GetContextLimit(
        const FCortexProviderDefinition& ProviderDefinition,
        const FString& ModelId);
    static TArray<FString> GetProviderOptions();
    static FString GetDefaultProviderId();
};
