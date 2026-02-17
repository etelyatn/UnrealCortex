#pragma once

#include "CoreMinimal.h"
#include "ICortexDomainHandler.h"

class CORTEXUMG_API FCortexUMGCommandHandler : public ICortexDomainHandler
{
public:
    virtual FCortexCommandResult Execute(
        const FString& Command,
        const TSharedPtr<FJsonObject>& Params,
        FDeferredResponseCallback DeferredCallback = nullptr
    ) override;

    virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;
};
