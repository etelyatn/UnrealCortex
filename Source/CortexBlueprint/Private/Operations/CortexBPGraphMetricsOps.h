// CortexBPGraphMetricsOps.h
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

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

	/** The entry's name exactly as graph_metrics reports it (event member name, custom event name, function name, bound event, input). */
	static FString EntryName(const UEdGraph* Graph, const UEdGraphNode* Node);

	/** One live entry of a graph and every node it reaches, including the nodes of the macro/composite instances it expands. */
	struct FLiveEntryReach
	{
		FString Name;
		FGuid NodeGuid;
		TSet<const UEdGraphNode*> Reached;
	};

	/**
	 * The live entries of one graph with the reachability graph_metrics measures them by (result seeding for pure or
	 * sole-unwired-result functions, data closure, collapsed and macro bodies expanded under the entry that reaches them). An inner
	 * graph's entry tunnel is a boundary, never an entry.
	 */
	static void CollectLiveEntryReaches(const UEdGraph* Graph, TArray<FLiveEntryReach>& OutEntries);
};
