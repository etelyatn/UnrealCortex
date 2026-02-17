#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexLevelDiscoveryOps
{
public:
    static FCortexCommandResult ListActorClasses(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult ListComponentClasses(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult DescribeClass(const TSharedPtr<FJsonObject>& Params);
};
