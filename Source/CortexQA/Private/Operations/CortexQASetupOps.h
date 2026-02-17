#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexQASetupOps
{
public:
    static FCortexCommandResult TeleportPlayer(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetActorProperty(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetRandomSeed(const TSharedPtr<FJsonObject>& Params);
};
