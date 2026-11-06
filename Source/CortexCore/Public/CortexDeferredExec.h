#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class CORTEXCORE_API FCortexDeferredExec
{
public:
	/** DeferredTimeoutSeconds > 0 overrides the transport's default wait for this command's answer. */
	static FCortexCommandResult RunNextTick(
		TFunction<FCortexCommandResult()>&& Work,
		FDeferredResponseCallback DeferredCallback,
		double DeferredTimeoutSeconds = 0.0);
};
