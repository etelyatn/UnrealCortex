#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexLevelComponentOps
{
public:
    static FCortexCommandResult ListComponents(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult AddComponent(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult RemoveComponent(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetComponentProperty(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetComponentProperty(const TSharedPtr<FJsonObject>& Params);
};
