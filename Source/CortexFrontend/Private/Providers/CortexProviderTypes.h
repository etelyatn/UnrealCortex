#pragma once

#include "CoreMinimal.h"
#include "Session/CortexSessionTypes.h"

struct FCortexProviderCapabilitySet
{
    bool bSupportsChat = false;
    bool bSupportsAuthAction = false;
};

struct FCortexProviderModelDefinition
{
    FString ModelId;
    FString DisplayName;
};

struct FCortexProviderDefinition
{
    FString ProviderId;
    FString DisplayName;
    FString DefaultModelId;
    ECortexEffortLevel DefaultEffort = ECortexEffortLevel::Default;
    FString AuthAction;
    FCortexProviderCapabilitySet Capabilities;
    TArray<FCortexProviderModelDefinition> Models;
};

struct FCortexResolvedSessionOptions
{
    FString ProviderId;
    FString ModelId;
    ECortexEffortLevel EffortLevel = ECortexEffortLevel::Default;
    FString AuthAction;
};
