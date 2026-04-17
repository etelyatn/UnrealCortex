#pragma once

#include "CoreMinimal.h"
#include "CortexProviderTypes.h"

class FCortexProviderRegistry
{
public:
    static const FCortexProviderDefinition* FindDefinition(const FString& ProviderId);
    static TArray<FString> GetProviderOptions();
    static FString GetDefaultProviderId();
};
