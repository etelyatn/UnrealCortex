#pragma once

#include "CoreMinimal.h"

/** Flow direction for layout */
enum class ECortexLayoutDirection : uint8
{
	LeftToRight,   // Blueprints: events on left, execution flows right
	RightToLeft    // Materials: result on right, sources flow left
};

/** Layout mode */
enum class ECortexLayoutMode : uint8
{
	Full,          // Reposition all nodes
	Incremental    // Only position new nodes (NodePosX==0 && NodePosY==0)
};

/** Abstract node for layout calculation — domain-agnostic */
struct FCortexLayoutNode
{
	FString Id;
	int32 Width = 150;
	int32 Height = 100;
	TArray<FString> ExecOutputs;   // IDs of nodes connected via execution pins
	TArray<FString> DataOutputs;   // IDs of nodes connected via data pins
	bool bIsEntryPoint = false;    // Event nodes, MaterialResult inputs, etc.
	bool bIsExecNode = false;      // Participates in execution flow (has exec pins)
};

/** Layout configuration */
struct FCortexLayoutConfig
{
	int32 HorizontalSpacing = 80;
	int32 VerticalSpacing = 40;
	ECortexLayoutDirection Direction = ECortexLayoutDirection::LeftToRight;
	ECortexLayoutMode Mode = ECortexLayoutMode::Full;
};

/** Result: node ID -> position */
struct FCortexLayoutResult
{
	TMap<FString, FIntPoint> Positions;  // NodeId -> (X, Y)
};

namespace CortexGraphLayout
{
	constexpr float InnerGroupHorizontalSpacingRatio = 0.3f;
	constexpr float InnerGroupVerticalSpacingRatio = 0.5f;
	constexpr int32 GridSnapSize = 16;
}

/** Shared layout engine — domain modules convert their nodes to/from this format */
class CORTEXGRAPH_API FCortexGraphLayoutOps
{
public:
	/**
	 * Calculate positions for all nodes in the graph.
	 * @param Nodes - Abstract node representations with connectivity
	 * @param Config - Layout configuration (spacing, direction, mode)
	 * @param ExistingPositions - Current positions (used by Incremental mode to skip placed nodes)
	 * @return Map of NodeId -> (X, Y) positions
	 */
	static FCortexLayoutResult CalculateLayout(
		const TArray<FCortexLayoutNode>& Nodes,
		const FCortexLayoutConfig& Config,
		const TMap<FString, FIntPoint>& ExistingPositions = TMap<FString, FIntPoint>()
	);

private:
	/** Assign each node to a layer (column) based on connectivity */
	static TMap<FString, int32> AssignLayers(
		const TArray<FCortexLayoutNode>& Nodes,
		ECortexLayoutDirection Direction
	);

	/** Order nodes within each layer to minimize edge crossings (barycenter heuristic) */
	static TMap<int32, TArray<FString>> OrderNodesInLayers(
		const TMap<FString, int32>& LayerAssignment,
		const TArray<FCortexLayoutNode>& Nodes
	);

	/** Calculate final X,Y positions using node dimensions and spacing */
	static FCortexLayoutResult CalculatePositions(
		const TMap<int32, TArray<FString>>& OrderedLayers,
		const TArray<FCortexLayoutNode>& Nodes,
		const FCortexLayoutConfig& Config
	);

	/** Find connected subgraphs for independent layout */
	static TArray<TArray<FString>> FindSubgraphs(const TArray<FCortexLayoutNode>& Nodes);

	/** Internal group representation for parameter group collapsing */
	struct FCortexNodeGroup
	{
		FString ExecNodeId;
		TArray<FString> DataNodeIds;
		int32 GroupWidth = 0;
		int32 GroupHeight = 0;
	};

	/** Discover parameter groups: BFS backward from exec nodes to claim pure data trees */
	static TArray<FCortexNodeGroup> DiscoverGroups(
		const TArray<FCortexLayoutNode>& Nodes,
		const TMap<FString, const FCortexLayoutNode*>& NodeMap,
		TMap<FString, int32>& OutNodeToGroupIndex
	);

	/** Build proxy nodes for top-level Sugiyama (one proxy per group + ungrouped nodes) */
	static TArray<FCortexLayoutNode> BuildGroupProxyNodes(
		const TArray<FCortexLayoutNode>& OriginalNodes,
		const TArray<FCortexNodeGroup>& Groups,
		const TMap<FString, int32>& NodeToGroupIndex,
		const TMap<FString, const FCortexLayoutNode*>& NodeMap,
		const FCortexLayoutConfig& Config
	);

	/** Expand group proxy positions into individual node positions */
	static void ExpandGroupPositions(
		const TArray<FCortexNodeGroup>& Groups,
		const TMap<FString, int32>& NodeToGroupIndex,
		const TMap<FString, const FCortexLayoutNode*>& NodeMap,
		const FCortexLayoutConfig& Config,
		FCortexLayoutResult& InOutResult
	);
};
