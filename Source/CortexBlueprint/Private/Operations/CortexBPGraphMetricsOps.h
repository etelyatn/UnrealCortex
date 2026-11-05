// CortexBPGraphMetricsOps.h
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPGraphMetricsOps
{
public:
	/**
	 * Reachability-aware logic metrics for every entry node (events, custom/bound/input events, function entries).
	 * Params: asset_path (string), thresholds (optional object: max_nodes_per_entry, max_nodes_per_blueprint — positive integers).
	 */
	static FCortexCommandResult GraphMetrics(const TSharedPtr<FJsonObject>& Params);
};
