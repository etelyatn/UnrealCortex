#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPTimelineOps
{
public:
	static FCortexCommandResult ConfigureTimeline(const TSharedPtr<FJsonObject>& Params);
};
