#pragma once

#include "CoreMinimal.h"
#include "Providers/CortexCliProvider.h"

class FCortexCodexCliProvider final : public ICortexCliProvider
{
public:
    virtual FName GetProviderId() const override;
    virtual const FCortexProviderDefinition& GetDefinition() const override;
    virtual ECortexCliTransportMode GetTransportMode() const override;
    virtual bool SupportsResume() const override;
    virtual FCortexCliInfo FindCli() const override;
    virtual FString BuildLaunchCommandLine(
        const FString& McpConfigPath,
        const FString& WorkingDirectory,
        const FString& SessionId,
        const FString& ModelId,
        ECortexEffortLevel EffortLevel,
        bool bBypassApprovals,
        bool bSkipPermissions,
        bool bResumeSession) const override;
    virtual FString BuildAuthCommand() const override;
    virtual void ConsumeStreamChunk(
        const FString& RawChunk,
        FString& InOutChunkBuffer,
        TArray<FCortexStreamEvent>& OutEvents) const override;
};
