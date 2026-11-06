// CortexBPGraphMetricsOps.h
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UBlueprint;

class FCortexBPGraphMetricsOps
{
public:
	/**
	 * Reachability-aware logic metrics for every entry node (events, custom/bound/input events, function entries).
	 * Params: asset_path (string), thresholds (optional object: max_nodes_per_entry, max_nodes_per_blueprint — positive integers).
	 */
	static FCortexCommandResult GraphMetrics(const TSharedPtr<FJsonObject>& Params);

	/** Violation limits; the defaults apply when a request carries no thresholds. */
	struct FThresholds
	{
		int32 MaxNodesPerEntry = 10;
		int32 MaxNodesPerBlueprint = 30;
	};

	/** The graph_metrics document of a loaded Blueprint; AssetPath is echoed as `asset_path`. Read-only. */
	static TSharedPtr<FJsonObject> BuildGraphMetrics(
		const UBlueprint* Blueprint, const FString& AssetPath, const FThresholds& Thresholds);
};
