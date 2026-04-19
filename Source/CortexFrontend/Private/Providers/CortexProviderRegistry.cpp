#include "Providers/CortexProviderRegistry.h"

namespace
{
    const FName DefaultProviderId(TEXT("claude_code"));

    const FCortexProviderModelDefinition* FindModelDefinition(
        const FCortexProviderDefinition& ProviderDefinition,
        const FString& ModelId)
    {
        return ProviderDefinition.Models.FindByPredicate(
            [&ModelId](const FCortexProviderModelDefinition& Model)
            {
                return Model.ModelId == ModelId;
            });
    }

    const FCortexProviderModelDefinition& GetFallbackModelDefinition(
        const FCortexProviderDefinition& ProviderDefinition)
    {
        const FCortexProviderModelDefinition* RecommendedModel = FindModelDefinition(
            ProviderDefinition,
            ProviderDefinition.RecommendedModelId);
        if (RecommendedModel != nullptr)
        {
            return *RecommendedModel;
        }

        check(ProviderDefinition.Models.Num() > 0);
        return ProviderDefinition.Models[0];
    }

    const TArray<FCortexProviderDefinition>& GetBuiltInDefinitions()
    {
        static const TArray<FCortexProviderDefinition> Definitions = []()
        {
            TArray<FCortexProviderDefinition> Result;

            FCortexProviderDefinition Claude;
            Claude.ProviderId = DefaultProviderId;
            Claude.DisplayName = TEXT("Claude Code");
            Claude.RecommendedModelId = TEXT("claude-sonnet-4-6");
            Claude.DefaultEffortLevel = ECortexEffortLevel::Default;
            Claude.SupportedEffortLevels =
            {
                ECortexEffortLevel::Default,
                ECortexEffortLevel::Low,
                ECortexEffortLevel::Medium,
                ECortexEffortLevel::High,
                ECortexEffortLevel::Maximum,
            };
            Claude.ExecutableDisplayName = TEXT("Claude CLI");
            Claude.InstallationHintText = TEXT("Install with: npm install -g @anthropic-ai/claude-code");
            Claude.AuthCommandDisplayText = TEXT("claude login");
            Claude.Capabilities.bSupportsChat = true;
            Claude.Capabilities.bSupportsAnalysis = true;
            Claude.Capabilities.bSupportsConversion = true;
            Claude.Capabilities.bSupportsQAGeneration = true;
            Claude.Capabilities.bSupportsAuthAction = true;
            Claude.Capabilities.bSupportsModelOverride = true;
            Claude.Capabilities.bSupportsEffortOverride = true;
            Claude.Capabilities.bSupportsContextLimitDisplay = true;
            Claude.Models =
            {
                { TEXT("claude-sonnet-4-6"), TEXT("claude-sonnet-4-6"), true, 200000, Claude.SupportedEffortLevels },
                { TEXT("claude-opus-4-6"), TEXT("claude-opus-4-6"), false, 200000, Claude.SupportedEffortLevels },
                { TEXT("claude-haiku-4-5-20251001"), TEXT("claude-haiku-4-5-20251001"), false, 200000, Claude.SupportedEffortLevels },
            };
            Result.Add(MoveTemp(Claude));

            FCortexProviderDefinition Codex;
            Codex.ProviderId = FName(TEXT("codex"));
            Codex.DisplayName = TEXT("Codex");
            Codex.RecommendedModelId = TEXT("gpt-5.4");
            Codex.DefaultEffortLevel = ECortexEffortLevel::Medium;
            Codex.SupportedEffortLevels =
            {
                ECortexEffortLevel::Low,
                ECortexEffortLevel::Medium,
                ECortexEffortLevel::High,
                ECortexEffortLevel::Maximum,
            };
            Codex.ExecutableDisplayName = TEXT("Codex CLI");
            Codex.InstallationHintText = TEXT("Install with: npm install -g @openai/codex");
            Codex.AuthCommandDisplayText = TEXT("codex login");
            Codex.Capabilities.bSupportsChat = true;
            Codex.Capabilities.bSupportsAnalysis = true;
            Codex.Capabilities.bSupportsConversion = true;
            Codex.Capabilities.bSupportsQAGeneration = true;
            Codex.Capabilities.bSupportsAuthAction = true;
            Codex.Capabilities.bSupportsModelOverride = true;
            Codex.Capabilities.bSupportsEffortOverride = true;
            Codex.Capabilities.bSupportsContextLimitDisplay = true;
            Codex.Models =
            {
                { TEXT("gpt-5.4"), TEXT("gpt-5.4"), true, 272000, Codex.SupportedEffortLevels },
                { TEXT("gpt-5.4-mini"), TEXT("GPT-5.4-Mini"), false, 272000, Codex.SupportedEffortLevels },
                { TEXT("gpt-5.3-codex"), TEXT("gpt-5.3-codex"), false, 272000, Codex.SupportedEffortLevels },
                { TEXT("gpt-5.3-codex-spark"), TEXT("GPT-5.3-Codex-Spark"), false, 128000, Codex.SupportedEffortLevels },
            };
            Result.Add(MoveTemp(Codex));

            return Result;
        }();

        return Definitions;
    }
}

const FCortexProviderDefinition* FCortexProviderRegistry::FindDefinition(const FString& ProviderId)
{
    const FName NormalizedProviderId(*ProviderId.ToLower());
    for (const FCortexProviderDefinition& Definition : GetBuiltInDefinitions())
    {
        if (Definition.ProviderId == NormalizedProviderId)
        {
            return &Definition;
        }
    }

    return nullptr;
}

const FCortexProviderDefinition& FCortexProviderRegistry::GetDefaultDefinition()
{
    const FCortexProviderDefinition* Definition = FindDefinition(DefaultProviderId.ToString());
    check(Definition != nullptr);
    return *Definition;
}

const FCortexProviderDefinition& FCortexProviderRegistry::ResolveDefinition(const FString& ProviderId)
{
    if (const FCortexProviderDefinition* Definition = FindDefinition(ProviderId))
    {
        return *Definition;
    }

    return GetDefaultDefinition();
}

const FCortexProviderModelDefinition& FCortexProviderRegistry::ValidateOrGetDefaultModel(
    const FCortexProviderDefinition& ProviderDefinition,
    const FString& ModelId)
{
    if (const FCortexProviderModelDefinition* ModelDefinition = FindModelDefinition(ProviderDefinition, ModelId))
    {
        return *ModelDefinition;
    }

    return GetFallbackModelDefinition(ProviderDefinition);
}

ECortexEffortLevel FCortexProviderRegistry::ValidateOrGetDefaultEffort(
    const FCortexProviderDefinition& ProviderDefinition,
    const FCortexProviderModelDefinition& ModelDefinition,
    ECortexEffortLevel EffortLevel)
{
    if (ProviderDefinition.SupportedEffortLevels.Contains(EffortLevel) &&
        ModelDefinition.SupportedEffortLevels.Contains(EffortLevel))
    {
        return EffortLevel;
    }

    if (ProviderDefinition.SupportedEffortLevels.Contains(ProviderDefinition.DefaultEffortLevel) &&
        ModelDefinition.SupportedEffortLevels.Contains(ProviderDefinition.DefaultEffortLevel))
    {
        return ProviderDefinition.DefaultEffortLevel;
    }

    for (const ECortexEffortLevel SupportedEffort : ModelDefinition.SupportedEffortLevels)
    {
        if (ProviderDefinition.SupportedEffortLevels.Contains(SupportedEffort))
        {
            return SupportedEffort;
        }
    }

    check(ModelDefinition.SupportedEffortLevels.Num() > 0);
    return ModelDefinition.SupportedEffortLevels[0];
}

int64 FCortexProviderRegistry::GetContextLimit(
    const FCortexProviderDefinition& ProviderDefinition,
    const FString& ModelId)
{
    if (const FCortexProviderModelDefinition* ModelDefinition = FindModelDefinition(ProviderDefinition, ModelId))
    {
        return ModelDefinition->ContextLimitTokens;
    }

    return GetFallbackModelDefinition(ProviderDefinition).ContextLimitTokens;
}

TArray<FString> FCortexProviderRegistry::GetProviderOptions()
{
    TArray<FString> Options;
    for (const FCortexProviderDefinition& Definition : GetBuiltInDefinitions())
    {
        Options.Add(Definition.ProviderId.ToString());
    }

    return Options;
}

FString FCortexProviderRegistry::GetDefaultProviderId()
{
    return GetDefaultDefinition().ProviderId.ToString();
}
