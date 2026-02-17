#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexQAActionOps
{
public:
    static FCortexCommandResult LookAt(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult Interact(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult MoveTo(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);
    static FCortexCommandResult WaitFor(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);
};
