#pragma once

#include "CoreMinimal.h"
#include "ICortexDomainHandler.h"

class CORTEXLEVEL_API FCortexLevelCommandHandler : public ICortexDomainHandler
{
public:
    virtual FCortexCommandResult Execute(
        const FString& Command,
        const TSharedPtr<FJsonObject>& Params
    ) override;

    virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;
};
