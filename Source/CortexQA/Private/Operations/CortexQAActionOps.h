#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexQAActionOps
{
public:
    static FCortexCommandResult LookAt(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult LookTo(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);
    static FCortexCommandResult CheckStuck(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);
    static FCortexCommandResult Interact(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);
    static FCortexCommandResult MoveTo(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);
    static FCortexCommandResult WaitFor(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);
};
