#pragma once

#include "CoreMinimal.h"
#include "Session/CortexSessionTypes.h"

struct FCortexProviderCapabilitySet
{
    bool bSupportsChat = false;
    bool bSupportsAnalysis = false;
    bool bSupportsConversion = false;
    bool bSupportsQAGeneration = false;
    bool bSupportsAuthAction = false;
    bool bSupportsModelOverride = false;
    bool bSupportsEffortOverride = false;
    bool bSupportsContextLimitDisplay = false;
};

struct FCortexProviderModelDefinition
{
    FString ModelId;
    FString DisplayName;
    bool bRecommendedDefault = false;
    int64 ContextLimitTokens = 0;
    TArray<ECortexEffortLevel> SupportedEffortLevels;
};

struct FCortexProviderDefinition
{
    FName ProviderId = NAME_None;
    FString DisplayName;
    FString RecommendedModelId;
    ECortexEffortLevel DefaultEffortLevel = ECortexEffortLevel::Default;
    TArray<ECortexEffortLevel> SupportedEffortLevels;
    TArray<FCortexProviderModelDefinition> Models;
    FCortexProviderCapabilitySet Capabilities;
    FString ExecutableDisplayName;
    FString InstallationHintText;
    FString AuthCommandDisplayText;
};

struct FCortexResolvedSessionOptions
{
    FName ProviderId = NAME_None;
    FString ProviderDisplayName;
    FString ModelId;
    ECortexEffortLevel EffortLevel = ECortexEffortLevel::Default;
    int64 ContextLimitTokens = 0;
};
