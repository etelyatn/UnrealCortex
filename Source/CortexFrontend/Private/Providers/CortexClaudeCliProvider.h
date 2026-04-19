#pragma once

#include "CoreMinimal.h"
#include "Providers/CortexCliProvider.h"

class FCortexClaudeCliProvider final : public ICortexCliProvider
{
public:
    virtual FName GetProviderId() const override;
    virtual const FCortexProviderDefinition& GetDefinition() const override;
    virtual ECortexCliTransportMode GetTransportMode() const override;
    virtual bool SupportsResume() const override;
    virtual FCortexCliInfo FindCli() const override;
    virtual FString BuildLaunchCommandLine(
        bool bResumeSession,
        ECortexAccessMode AccessMode,
        const FCortexSessionConfig& SessionConfig) const override;
    virtual FString BuildPromptEnvelope(
        const FString& Prompt,
        ECortexAccessMode AccessMode,
        const FCortexSessionConfig& SessionConfig) const override;
    virtual FString BuildAuthCommand() const override;
    virtual void ConsumeStreamChunk(
        const FString& RawChunk,
        FString& InOutChunkBuffer,
        FString& InOutAssistantText,
        TArray<FCortexStreamEvent>& OutEvents) const override;
};
