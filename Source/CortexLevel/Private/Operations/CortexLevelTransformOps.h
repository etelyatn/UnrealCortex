#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexLevelTransformOps
{
public:
    static FCortexCommandResult GetActor(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetTransform(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetActorProperty(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetActorProperty(const TSharedPtr<FJsonObject>& Params);
};
