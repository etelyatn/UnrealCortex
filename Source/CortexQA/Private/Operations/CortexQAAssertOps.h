#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexQAAssertOps
{
public:
    static FCortexCommandResult AssertState(const TSharedPtr<FJsonObject>& Params);
};
