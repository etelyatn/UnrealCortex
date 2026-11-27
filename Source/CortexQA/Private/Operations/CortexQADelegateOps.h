#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexQADelegateOps
{
public:
    /** qa.broadcast_delegate: broadcast a multicast delegate (inline or sparse) on a PIE actor or one of its components. */
    static FCortexCommandResult BroadcastDelegate(const TSharedPtr<FJsonObject>& Params);
};
