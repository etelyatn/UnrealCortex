#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexQAWorldOps
{
public:
    static FCortexCommandResult ObserveState(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetActorState(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetPlayerState(const TSharedPtr<FJsonObject>& Params);
};
