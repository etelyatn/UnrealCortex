#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexLevelQueryOps
{
public:
    static FCortexCommandResult ListActors(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult FindActors(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetBounds(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SelectActors(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetSelection(const TSharedPtr<FJsonObject>& Params);
};
