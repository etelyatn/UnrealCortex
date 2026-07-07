#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class CORTEXCORE_API FCortexDeferredExec
{
public:
	static FCortexCommandResult RunNextTick(
		TFunction<FCortexCommandResult()>&& Work,
		FDeferredResponseCallback DeferredCallback);
};
