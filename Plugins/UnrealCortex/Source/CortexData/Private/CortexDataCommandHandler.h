#pragma once

#include "CoreMinimal.h"
#include "ICortexDomainHandler.h"

class FCortexDataCommandHandler : public ICortexDomainHandler
{
public:
    virtual FUDBCommandResult Execute(
        const FString& Command,
        const TSharedPtr<FJsonObject>& Params
    ) override;

    virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;
};
