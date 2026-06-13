#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Process/CortexStreamEvent.h"
#include "Session/CortexSessionTypes.h"

struct FCortexCodexAppServerProtocolState
{
    FString ThreadId;
    FString ActiveTurnId;
    FString PendingAssistantText;
};

class FCortexCodexAppServerProtocol
{
public:
    static FString BuildInitializeRequest(int32 RequestId);
    static FString BuildInitializedNotification();
    static FString BuildThreadStartRequest(
        int32 RequestId,
        const FCortexSessionConfig& Config,
        ECortexAccessMode AccessMode);
    static FString BuildTurnStartRequest(
        int32 RequestId,
        const FString& ThreadId,
        const FString& Prompt,
        const FCortexSessionConfig& Config,
        ECortexAccessMode AccessMode);
    static FString BuildTurnInterruptRequest(int32 RequestId, const FString& ThreadId);

    static bool ParseLine(
        const FString& JsonLine,
        FCortexCodexAppServerProtocolState& State,
        TArray<FCortexStreamEvent>& OutEvents);

private:
    static FString AccessModeToSandboxMode(ECortexAccessMode AccessMode);
    static TSharedPtr<FJsonObject> AccessModeToSandboxPolicy(ECortexAccessMode AccessMode);
    static FString EffortToAppServerValue(ECortexEffortLevel EffortLevel);
};
