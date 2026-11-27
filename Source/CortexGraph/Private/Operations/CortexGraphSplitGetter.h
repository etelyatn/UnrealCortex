#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FJsonObject;

/**
 * graph.split_getter: gives every consumer node of a pure Variable Get beyond the first (by link order) its own get
 * node, so one getter wired across a whole graph becomes one getter per consumer. One transaction; dry_run previews.
 */
class FCortexGraphSplitGetter
{
public:
	static FCortexCommandResult Execute(const TSharedPtr<FJsonObject>& Params);
};
