#pragma once

#include "CoreMinimal.h"
#include "ICortexDomainHandler.h"

class CORTEXSTATETREE_API FCortexStateTreeCommandHandler : public ICortexDomainHandler
{
public:
    virtual FCortexCommandResult Execute(
        const FString& Command,
        const TSharedPtr<FJsonObject>& Params,
        FDeferredResponseCallback DeferredCallback = nullptr
    ) override;

    virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;

private:
    FCortexCommandResult GetStatus(const TSharedPtr<FJsonObject>& Params) const;
};
