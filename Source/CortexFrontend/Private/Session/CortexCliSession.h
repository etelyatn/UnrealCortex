#pragma once

#include "CoreMinimal.h"
#include "Process/CortexCliDiscovery.h"
#include "Session/CortexSessionTypes.h"

class FCortexCliSession : public TSharedFromThis<FCortexCliSession>
{
public:
    explicit FCortexCliSession(const FCortexSessionConfig& InConfig);

private:
    friend class FCortexCliSessionBuildInitialLaunchArgsTest;
    friend class FCortexCliSessionBuildResumeLaunchArgsTest;
    friend class FCortexCliSessionBuildPromptEnvelopeTest;

    FString BuildLaunchCommandLine(bool bResumeSession, ECortexAccessMode AccessMode) const;
    FString BuildAllowedToolsArg(ECortexAccessMode AccessMode) const;
    FString BuildPromptEnvelope(const FString& Prompt) const;

    FCortexSessionConfig Config;
    FCortexCliInfo CachedCliInfo;
    std::atomic<ECortexSessionState> State;
};
