#pragma once

#include "CoreMinimal.h"
#include "ICortexDomainHandler.h"
#include "Recording/CortexQASessionTypes.h"

class FCortexQARecorder;
class FCortexQAReplaySequencer;

class CORTEXQA_API FCortexQACommandHandler : public ICortexDomainHandler, public TSharedFromThis<FCortexQACommandHandler>
{
public:
    FCortexQACommandHandler();
    virtual ~FCortexQACommandHandler();

    virtual FCortexCommandResult Execute(
        const FString& Command,
        const TSharedPtr<FJsonObject>& Params,
        FDeferredResponseCallback DeferredCallback = nullptr
    ) override;

    virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;

    ECortexQASessionState GetSessionState() const { return SessionState; }

private:
    FCortexCommandResult StartRecording(const TSharedPtr<FJsonObject>& Params);
    FCortexCommandResult StopRecordingCmd(const TSharedPtr<FJsonObject>& Params);
    FCortexCommandResult ReplaySession(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);
    FCortexCommandResult CancelReplay(const TSharedPtr<FJsonObject>& Params);

    ECortexQASessionState SessionState = ECortexQASessionState::Idle;
    TSharedPtr<FCortexQARecorder> Recorder;
    TSharedPtr<FCortexQAReplaySequencer> ActiveSequencer;
};
