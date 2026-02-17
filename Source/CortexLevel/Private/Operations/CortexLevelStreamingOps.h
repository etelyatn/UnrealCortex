#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexLevelStreamingOps
{
public:
    static FCortexCommandResult GetInfo(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult ListSublevels(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult LoadSublevel(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult UnloadSublevel(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetSublevelVisibility(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult ListDataLayers(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetDataLayer(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SaveLevel(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SaveAll(const TSharedPtr<FJsonObject>& Params);
};
