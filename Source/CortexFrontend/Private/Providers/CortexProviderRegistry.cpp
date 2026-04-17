#include "Providers/CortexProviderRegistry.h"

namespace
{
    const TArray<FCortexProviderDefinition>& GetBuiltInDefinitions()
    {
        static const TArray<FCortexProviderDefinition> Definitions = []()
        {
            TArray<FCortexProviderDefinition> Result;

            FCortexProviderDefinition Claude;
            Claude.ProviderId = TEXT("claude_code");
            Claude.DisplayName = TEXT("Claude Code");
            Claude.DefaultModelId = TEXT("claude-sonnet-4-6");
            Claude.DefaultEffort = ECortexEffortLevel::Default;
            Claude.AuthAction = TEXT("claude login");
            Claude.Capabilities.bSupportsChat = true;
            Claude.Capabilities.bSupportsAuthAction = true;
            Claude.Models =
            {
                { TEXT("claude-sonnet-4-6"), TEXT("Claude Sonnet 4.6") },
            };
            Result.Add(MoveTemp(Claude));

            FCortexProviderDefinition Codex;
            Codex.ProviderId = TEXT("codex");
            Codex.DisplayName = TEXT("Codex");
            Codex.DefaultModelId = TEXT("gpt-5.4");
            Codex.DefaultEffort = ECortexEffortLevel::Medium;
            Codex.AuthAction = TEXT("codex login");
            Codex.Capabilities.bSupportsChat = true;
            Codex.Capabilities.bSupportsAuthAction = true;
            Codex.Models =
            {
                { TEXT("gpt-5.4"), TEXT("GPT-5.4") },
                { TEXT("gpt-5.4-mini"), TEXT("GPT-5.4 Mini") },
                { TEXT("gpt-5.3-codex"), TEXT("GPT-5.3 Codex") },
                { TEXT("gpt-5.3-codex-spark"), TEXT("GPT-5.3 Codex Spark") },
            };
            Result.Add(MoveTemp(Codex));

            return Result;
        }();

        return Definitions;
    }
}

const FCortexProviderDefinition* FCortexProviderRegistry::FindDefinition(const FString& ProviderId)
{
    const FString NormalizedProviderId = ProviderId.ToLower();
    for (const FCortexProviderDefinition& Definition : GetBuiltInDefinitions())
    {
        if (Definition.ProviderId.ToLower() == NormalizedProviderId)
        {
            return &Definition;
        }
    }

    return nullptr;
}
