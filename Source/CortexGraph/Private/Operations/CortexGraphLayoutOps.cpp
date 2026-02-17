#include "CortexGraphLayoutOps.h"
#include "CortexGraphModule.h"

FCortexLayoutResult FCortexGraphLayoutOps::CalculateLayout(
	const TArray<FCortexLayoutNode>& Nodes,
	const FCortexLayoutConfig& Config,
	const TMap<FString, FIntPoint>& ExistingPositions)
{
	if (Nodes.Num() == 0)
	{
		return FCortexLayoutResult();
	}

	// Build lookup map
	TMap<FString, const FCortexLayoutNode*> NodeMap;
	for (const FCortexLayoutNode& Node : Nodes)
	{
		NodeMap.Add(Node.Id, &Node);
	}

	// Step 1: Find subgraphs
	TArray<TArray<FString>> Subgraphs = FindSubgraphs(Nodes);

	// Step 2: Layout each subgraph independently
	FCortexLayoutResult FinalResult;
	int32 SubgraphOffsetY = 0;

	for (const TArray<FString>& SubgraphIds : Subgraphs)
	{
		// Filter nodes for this subgraph
		TArray<FCortexLayoutNode> SubNodes;
		for (const FString& Id : SubgraphIds)
		{
			if (const FCortexLayoutNode* const* Found = NodeMap.Find(Id))
			{
				SubNodes.Add(**Found);
			}
		}

		// Assign layers
		TMap<FString, int32> LayerAssignment = AssignLayers(SubNodes, Config.Direction);

		// Order within layers
		TMap<int32, TArray<FString>> OrderedLayers = OrderNodesInLayers(LayerAssignment, SubNodes);

		// Calculate positions
		FCortexLayoutResult SubResult = CalculatePositions(OrderedLayers, SubNodes, Config);

		// Offset subgraph vertically
		int32 MaxY = 0;
		for (auto& Pair : SubResult.Positions)
		{
			Pair.Value.Y += SubgraphOffsetY;
			FinalResult.Positions.Add(Pair.Key, Pair.Value);

			// Track max Y for next subgraph offset
			const FCortexLayoutNode* const* NodePtr = NodeMap.Find(Pair.Key);
			int32 NodeBottom = Pair.Value.Y + (NodePtr ? (*NodePtr)->Height : 100);
			if (NodeBottom > MaxY)
			{
				MaxY = NodeBottom;
			}
		}

		static constexpr int32 SubgraphGapMultiplier = 3;
		SubgraphOffsetY = MaxY + Config.VerticalSpacing * SubgraphGapMultiplier;
	}

	// Incremental mode: only keep positions for nodes that had default (0,0) position
	if (Config.Mode == ECortexLayoutMode::Incremental)
	{
		FCortexLayoutResult FilteredResult;
		for (const auto& Pair : FinalResult.Positions)
		{
			const FIntPoint* Existing = ExistingPositions.Find(Pair.Key);
			if (!Existing || (Existing->X == 0 && Existing->Y == 0))
			{
				FilteredResult.Positions.Add(Pair.Key, Pair.Value);
			}
		}
		return FilteredResult;
	}

	return FinalResult;
}

TMap<FString, int32> FCortexGraphLayoutOps::AssignLayers(
	const TArray<FCortexLayoutNode>& Nodes,
	ECortexLayoutDirection Direction)
{
	TMap<FString, int32> LayerAssignment;
	TMap<FString, const FCortexLayoutNode*> NodeMap;
	for (const FCortexLayoutNode& Node : Nodes)
	{
		NodeMap.Add(Node.Id, &Node);
	}

	// Build reverse adjacency: for each node, who connects TO it
	TMap<FString, TArray<FString>> IncomingExec;
	TMap<FString, TArray<FString>> IncomingData;
	for (const FCortexLayoutNode& Node : Nodes)
	{
		for (const FString& TargetId : Node.ExecOutputs)
		{
			IncomingExec.FindOrAdd(TargetId).AddUnique(Node.Id);
		}
		for (const FString& TargetId : Node.DataOutputs)
		{
			IncomingData.FindOrAdd(TargetId).AddUnique(Node.Id);
		}
	}

	// --- Topological sort + forward longest-path pass ---
	// Compute in-degrees for exec edges
	TMap<FString, int32> InDegree;
	for (const FCortexLayoutNode& Node : Nodes)
	{
		InDegree.FindOrAdd(Node.Id); // Ensure all nodes present
		for (const FString& TargetId : Node.ExecOutputs)
		{
			InDegree.FindOrAdd(TargetId)++;
		}
	}

	// Initialize — entry points and zero in-degree exec-connected nodes
	TArray<FString> TopoQueue;
	for (const FCortexLayoutNode& Node : Nodes)
	{
		bool bHasExecEdge = (Node.ExecOutputs.Num() > 0) || IncomingExec.Contains(Node.Id);
		if (Node.bIsEntryPoint || (bHasExecEdge && InDegree.FindRef(Node.Id) == 0))
		{
			TopoQueue.Add(Node.Id);
			LayerAssignment.Add(Node.Id, 0);
		}
	}

	// Fallback: if no roots found (all cycles), pick first exec-connected node
	if (TopoQueue.Num() == 0)
	{
		for (const FCortexLayoutNode& Node : Nodes)
		{
			if (Node.ExecOutputs.Num() > 0 || IncomingExec.Contains(Node.Id))
			{
				TopoQueue.Add(Node.Id);
				LayerAssignment.Add(Node.Id, 0);
				break;
			}
		}
	}

	// Kahn's algorithm — process nodes in topological order
	int32 QueueIndex = 0;
	while (QueueIndex < TopoQueue.Num())
	{
		FString CurrentId = TopoQueue[QueueIndex++];
		int32 CurrentLayer = LayerAssignment.FindRef(CurrentId);
		const FCortexLayoutNode* const* CurrentPtr = NodeMap.Find(CurrentId);
		if (!CurrentPtr)
		{
			continue;
		}

		for (const FString& TargetId : (*CurrentPtr)->ExecOutputs)
		{
			// Propagate longest path
			int32 NewLayer = CurrentLayer + 1;
			int32& TargetLayer = LayerAssignment.FindOrAdd(TargetId);
			if (NewLayer > TargetLayer)
			{
				TargetLayer = NewLayer;
			}

			// Decrement in-degree; enqueue when all predecessors processed
			int32& Deg = InDegree.FindOrAdd(TargetId);
			Deg--;
			if (Deg == 0)
			{
				TopoQueue.Add(TargetId);
			}
		}
	}

	// --- Data-flow fallback for pure data-flow graphs (e.g., Materials) ---
	int32 ExecAssignedCount = LayerAssignment.Num();
	bool bHasUnassignedDataNodes = false;
	for (const FCortexLayoutNode& Node : Nodes)
	{
		if (!LayerAssignment.Contains(Node.Id) && Node.DataOutputs.Num() > 0)
		{
			bHasUnassignedDataNodes = true;
			break;
		}
	}

	if (bHasUnassignedDataNodes || ExecAssignedCount <= 1)
	{
		// Count total exec edges — if zero, this is a pure data-flow graph
		int32 TotalExecEdges = 0;
		for (const FCortexLayoutNode& Node : Nodes)
		{
			TotalExecEdges += Node.ExecOutputs.Num();
		}

		if (TotalExecEdges == 0)
		{
			// Pure data-flow graph: run Kahn's on DataOutputs
			TMap<FString, int32> DataInDegree;
			for (const FCortexLayoutNode& Node : Nodes)
			{
				DataInDegree.FindOrAdd(Node.Id);
				for (const FString& TargetId : Node.DataOutputs)
				{
					DataInDegree.FindOrAdd(TargetId)++;
				}
			}

			TArray<FString> DataQueue;
			for (const FCortexLayoutNode& Node : Nodes)
			{
				if (DataInDegree.FindRef(Node.Id) == 0)
				{
					DataQueue.Add(Node.Id);
					LayerAssignment.FindOrAdd(Node.Id) = 0;
				}
			}

			int32 DataQueueIdx = 0;
			while (DataQueueIdx < DataQueue.Num())
			{
				FString CurId = DataQueue[DataQueueIdx++];
				int32 CurLayer = LayerAssignment.FindRef(CurId);
				const FCortexLayoutNode* const* CurPtr = NodeMap.Find(CurId);
				if (!CurPtr)
				{
					continue;
				}

				for (const FString& TgtId : (*CurPtr)->DataOutputs)
				{
					int32 NewLayer = CurLayer + 1;
					int32& TgtLayer = LayerAssignment.FindOrAdd(TgtId);
					if (NewLayer > TgtLayer)
					{
						TgtLayer = NewLayer;
					}

					int32& DDeg = DataInDegree.FindOrAdd(TgtId);
					DDeg--;
					if (DDeg == 0)
					{
						DataQueue.Add(TgtId);
					}
				}
			}
		}
	}

	// Handle exec-connected nodes still not assigned (cycles)
	for (const FCortexLayoutNode& Node : Nodes)
	{
		if (!LayerAssignment.Contains(Node.Id))
		{
			bool bHasExecEdge = (Node.ExecOutputs.Num() > 0) || IncomingExec.Contains(Node.Id);
			if (bHasExecEdge)
			{
				LayerAssignment.Add(Node.Id, 0);
			}
		}
	}

	// Place data-only nodes: in the column before their rightmost consumer (MaxConsumerLayer)
	for (const FCortexLayoutNode& Node : Nodes)
	{
		if (LayerAssignment.Contains(Node.Id))
		{
			continue;
		}

		int32 MaxConsumerLayer = -1;
		for (const FString& TargetId : Node.DataOutputs)
		{
			const int32* TargetLayer = LayerAssignment.Find(TargetId);
			if (TargetLayer && *TargetLayer > MaxConsumerLayer)
			{
				MaxConsumerLayer = *TargetLayer;
			}
		}

		if (MaxConsumerLayer >= 0)
		{
			LayerAssignment.Add(Node.Id, FMath::Max(0, MaxConsumerLayer - 1));
		}
		else
		{
			LayerAssignment.Add(Node.Id, 0);
		}
	}

	// For RightToLeft direction, invert layers so entry points move rightmost.
	// Only apply to exec-flow graphs — pure data-flow Kahn's already produces
	// sources-left / sinks-right ordering naturally.
	if (Direction == ECortexLayoutDirection::RightToLeft)
	{
		int32 TotalExecEdgesForInversion = 0;
		for (const FCortexLayoutNode& Node : Nodes)
		{
			TotalExecEdgesForInversion += Node.ExecOutputs.Num();
		}

		if (TotalExecEdgesForInversion > 0)
		{
			int32 MaxLayer = 0;
			for (const auto& Pair : LayerAssignment)
			{
				if (Pair.Value > MaxLayer)
				{
					MaxLayer = Pair.Value;
				}
			}
			for (auto& Pair : LayerAssignment)
			{
				Pair.Value = MaxLayer - Pair.Value;
			}
		}
	}

	return LayerAssignment;
}

TMap<int32, TArray<FString>> FCortexGraphLayoutOps::OrderNodesInLayers(
	const TMap<FString, int32>& LayerAssignment,
	const TArray<FCortexLayoutNode>& Nodes)
{
	// Group nodes by layer
	TMap<int32, TArray<FString>> Layers;
	for (const auto& Pair : LayerAssignment)
	{
		Layers.FindOrAdd(Pair.Value).Add(Pair.Key);
	}

	// Build adjacency for barycenter calculation
	TMap<FString, TArray<FString>> AllConnections;
	for (const FCortexLayoutNode& Node : Nodes)
	{
		TArray<FString>& Conns = AllConnections.FindOrAdd(Node.Id);
		Conns.Append(Node.ExecOutputs);
		Conns.Append(Node.DataOutputs);
	}

	// Also build reverse connections
	TMap<FString, TArray<FString>> ReverseConnections;
	for (const FCortexLayoutNode& Node : Nodes)
	{
		for (const FString& TargetId : Node.ExecOutputs)
		{
			ReverseConnections.FindOrAdd(TargetId).AddUnique(Node.Id);
		}
		for (const FString& TargetId : Node.DataOutputs)
		{
			ReverseConnections.FindOrAdd(TargetId).AddUnique(Node.Id);
		}
	}

	// Pre-build entry point lookup
	TMap<FString, bool> EntryPointMap;
	for (const FCortexLayoutNode& N : Nodes)
	{
		EntryPointMap.Add(N.Id, N.bIsEntryPoint);
	}

	// Track Y-order indices for barycenter
	TMap<FString, float> NodeYOrder;
	for (auto& LayerPair : Layers)
	{
		TArray<FString>& LayerNodes = LayerPair.Value;
		LayerNodes.Sort([&EntryPointMap](const FString& A, const FString& B)
		{
			bool AEntry = EntryPointMap.FindRef(A);
			bool BEntry = EntryPointMap.FindRef(B);
			if (AEntry != BEntry)
			{
				return AEntry;
			}
			return A < B;
		});

		for (int32 i = 0; i < LayerNodes.Num(); ++i)
		{
			NodeYOrder.Add(LayerNodes[i], static_cast<float>(i));
		}
	}

	// Barycenter iterations (4 passes: forward/backward alternating)
	for (int32 Pass = 0; Pass < 4; ++Pass)
	{
		TArray<int32> LayerKeys;
		Layers.GetKeys(LayerKeys);
		LayerKeys.Sort();

		bool bForward = (Pass % 2 == 0);
		if (!bForward)
		{
			Algo::Reverse(LayerKeys);
		}

		for (int32 LayerIdx : LayerKeys)
		{
			TArray<FString>& LayerNodes = Layers[LayerIdx];

			for (const FString& NodeId : LayerNodes)
			{
				float Sum = 0.0f;
				int32 Count = 0;

				const TArray<FString>* RevConns = ReverseConnections.Find(NodeId);
				if (RevConns)
				{
					for (const FString& ConnId : *RevConns)
					{
						const float* Order = NodeYOrder.Find(ConnId);
						if (Order)
						{
							Sum += *Order;
							++Count;
						}
					}
				}

				const TArray<FString>* FwdConns = AllConnections.Find(NodeId);
				if (FwdConns)
				{
					for (const FString& ConnId : *FwdConns)
					{
						const float* Order = NodeYOrder.Find(ConnId);
						if (Order)
						{
							Sum += *Order;
							++Count;
						}
					}
				}

				if (Count > 0)
				{
					NodeYOrder[NodeId] = Sum / static_cast<float>(Count);
				}
			}

			LayerNodes.Sort([&NodeYOrder](const FString& A, const FString& B)
			{
				return NodeYOrder.FindRef(A) < NodeYOrder.FindRef(B);
			});

			for (int32 i = 0; i < LayerNodes.Num(); ++i)
			{
				NodeYOrder[LayerNodes[i]] = static_cast<float>(i);
			}
		}
	}

	return Layers;
}

FCortexLayoutResult FCortexGraphLayoutOps::CalculatePositions(
	const TMap<int32, TArray<FString>>& OrderedLayers,
	const TArray<FCortexLayoutNode>& Nodes,
	const FCortexLayoutConfig& Config)
{
	TMap<FString, const FCortexLayoutNode*> NodeMap;
	for (const FCortexLayoutNode& Node : Nodes)
	{
		NodeMap.Add(Node.Id, &Node);
	}

	FCortexLayoutResult Result;

	TArray<int32> LayerKeys;
	OrderedLayers.GetKeys(LayerKeys);
	LayerKeys.Sort();

	int32 CurrentX = 0;
	TMap<int32, int32> LayerX;
	TMap<int32, int32> LayerMaxWidth;

	for (int32 LayerIdx : LayerKeys)
	{
		int32 MaxWidth = 0;
		const TArray<FString>& LayerNodes = OrderedLayers[LayerIdx];
		for (const FString& NodeId : LayerNodes)
		{
			const FCortexLayoutNode* const* NodePtr = NodeMap.Find(NodeId);
			int32 W = NodePtr ? (*NodePtr)->Width : 150;
			if (W > MaxWidth)
			{
				MaxWidth = W;
			}
		}

		LayerX.Add(LayerIdx, CurrentX);
		LayerMaxWidth.Add(LayerIdx, MaxWidth);
		CurrentX += MaxWidth + Config.HorizontalSpacing;
	}

	for (int32 LayerIdx : LayerKeys)
	{
		const TArray<FString>& LayerNodes = OrderedLayers[LayerIdx];
		int32 X = LayerX[LayerIdx];

		int32 TotalHeight = 0;
		for (int32 i = 0; i < LayerNodes.Num(); ++i)
		{
			const FCortexLayoutNode* const* NodePtr = NodeMap.Find(LayerNodes[i]);
			int32 H = NodePtr ? (*NodePtr)->Height : 100;
			TotalHeight += H;
			if (i < LayerNodes.Num() - 1)
			{
				TotalHeight += Config.VerticalSpacing;
			}
		}

		int32 StartY = -TotalHeight / 2;
		int32 CurrentY = StartY;

		for (const FString& NodeId : LayerNodes)
		{
			const FCortexLayoutNode* const* NodePtr = NodeMap.Find(NodeId);
			int32 H = NodePtr ? (*NodePtr)->Height : 100;

			Result.Positions.Add(NodeId, FIntPoint(X, CurrentY));
			CurrentY += H + Config.VerticalSpacing;
		}
	}

	return Result;
}

TArray<TArray<FString>> FCortexGraphLayoutOps::FindSubgraphs(const TArray<FCortexLayoutNode>& Nodes)
{
	TMap<FString, TSet<FString>> Adjacency;
	for (const FCortexLayoutNode& Node : Nodes)
	{
		TSet<FString>& Adj = Adjacency.FindOrAdd(Node.Id);
		for (const FString& TargetId : Node.ExecOutputs)
		{
			Adj.Add(TargetId);
			Adjacency.FindOrAdd(TargetId).Add(Node.Id);
		}
		for (const FString& TargetId : Node.DataOutputs)
		{
			Adj.Add(TargetId);
			Adjacency.FindOrAdd(TargetId).Add(Node.Id);
		}
	}

	TArray<TArray<FString>> Subgraphs;
	TSet<FString> GlobalVisited;

	for (const FCortexLayoutNode& Node : Nodes)
	{
		if (GlobalVisited.Contains(Node.Id))
		{
			continue;
		}

		TArray<FString> Component;
		TArray<FString> Queue;
		Queue.Add(Node.Id);
		GlobalVisited.Add(Node.Id);

		int32 QueueIndex = 0;
		while (QueueIndex < Queue.Num())
		{
			FString CurrentId = Queue[QueueIndex++];
			Component.Add(CurrentId);

			const TSet<FString>* Neighbors = Adjacency.Find(CurrentId);
			if (Neighbors)
			{
				for (const FString& NeighborId : *Neighbors)
				{
					if (!GlobalVisited.Contains(NeighborId))
					{
						GlobalVisited.Add(NeighborId);
						Queue.Add(NeighborId);
					}
				}
			}
		}

		Subgraphs.Add(MoveTemp(Component));
	}

	return Subgraphs;
}
