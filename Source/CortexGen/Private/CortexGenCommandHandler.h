#pragma once

#include "CoreMinimal.h"
#include "ICortexDomainHandler.h"
#include "CortexGenTypes.h"

class FCortexGenJobManager;

class CORTEXGEN_API FCortexGenCommandHandler : public ICortexDomainHandler
{
public:
    explicit FCortexGenCommandHandler(TSharedPtr<FCortexGenJobManager> InJobManager);

    virtual FCortexCommandResult Execute(
        const FString& Command,
        const TSharedPtr<FJsonObject>& Params,
        FDeferredResponseCallback DeferredCallback = nullptr
    ) override;

    virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;

private:
    FCortexCommandResult HandleStartMesh(const TSharedPtr<FJsonObject>& Params);
    FCortexCommandResult HandleStartImage(const TSharedPtr<FJsonObject>& Params);
    FCortexCommandResult HandleStartTexturing(const TSharedPtr<FJsonObject>& Params);
    FCortexCommandResult HandleJobStatus(const TSharedPtr<FJsonObject>& Params);
    FCortexCommandResult HandleListJobs(const TSharedPtr<FJsonObject>& Params);
    FCortexCommandResult HandleCancelJob(const TSharedPtr<FJsonObject>& Params);
    FCortexCommandResult HandleRetryImport(const TSharedPtr<FJsonObject>& Params);
    FCortexCommandResult HandleListProviders(const TSharedPtr<FJsonObject>& Params);
    FCortexCommandResult HandleDeleteJob(const TSharedPtr<FJsonObject>& Params);
    FCortexCommandResult HandleGetConfig(const TSharedPtr<FJsonObject>& Params);

    FCortexCommandResult SubmitGenJob(ECortexGenJobType Type, const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FCortexGenJobManager> JobManager;
};
