#pragma once

#include "CoreMinimal.h"
#include "Providers/CortexProviderTypes.h"
#include "Process/CortexStreamEvent.h"
#include "Session/CortexSessionTypes.h"

struct FCortexCliInfo
{
    FName ProviderId = NAME_None;
    FString ProviderDisplayName;
    FString Path;
    bool bIsCmd = false;
    bool bIsValid = false;
};

enum class ECortexCliTransportMode : uint8
{
    PersistentSession,
    PerTurnExec
};

class ICortexCliProvider
{
public:
    virtual ~ICortexCliProvider() = default;

    virtual FName GetProviderId() const = 0;
    virtual const FCortexProviderDefinition& GetDefinition() const = 0;
    virtual ECortexCliTransportMode GetTransportMode() const = 0;
    virtual bool SupportsResume() const = 0;
    virtual FCortexCliInfo FindCli() const = 0;
    virtual FString BuildLaunchCommandLine(
        const FString& McpConfigPath,
        const FString& WorkingDirectory,
        const FString& SessionId,
        const FString& ModelId,
        ECortexEffortLevel EffortLevel,
        bool bBypassApprovals,
        bool bSkipPermissions,
        bool bResumeSession) const = 0;
    virtual FString BuildAuthCommand() const = 0;
    virtual void ConsumeStreamChunk(
        const FString& RawChunk,
        FString& InOutChunkBuffer,
        TArray<FCortexStreamEvent>& OutEvents) const = 0;
};
