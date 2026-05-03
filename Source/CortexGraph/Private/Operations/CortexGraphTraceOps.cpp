#include "Operations/CortexGraphTraceOps.h"

#include "Operations/CortexGraphNodeOps.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"

namespace
{
struct FCortexResolvedGraphNode
{
	UEdGraph* Graph = nullptr;
	UEdGraphNode* Node = nullptr;
	FString GraphName;
	FString SubgraphPath;
};

struct FCortexTraceOptions
{
	FString GraphName;
	FString SubgraphPath;
	int32 MaxDepth = 10;
	bool bIncludeEdges = true;
	bool bExecOnly = true;
};

void CollectGraphsRecursive(
	UEdGraph* Graph,
	const FString& CurrentSubgraphPath,
	TArray<TPair<UEdGraph*, FString>>& OutGraphs,
	int32 Depth)
{
	if (!Graph || Depth > 5)
	{
		return;
	}

	OutGraphs.Emplace(Graph, CurrentSubgraphPath);

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		const UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node);
		if (!CompositeNode || !CompositeNode->BoundGraph)
		{
			continue;
		}

		const FString ChildPath = CurrentSubgraphPath.IsEmpty()
			? CompositeNode->BoundGraph->GetName()
			: FString::Printf(TEXT("%s.%s"), *CurrentSubgraphPath, *CompositeNode->BoundGraph->GetName());
		CollectGraphsRecursive(CompositeNode->BoundGraph, ChildPath, OutGraphs, Depth + 1);
	}
}

void GetAllCandidateGraphs(
	UBlueprint* Blueprint,
	TArray<TPair<UEdGraph*, FString>>& OutGraphs)
{
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		CollectGraphsRecursive(Graph, TEXT(""), OutGraphs, 0);
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		CollectGraphsRecursive(Graph, TEXT(""), OutGraphs, 0);
	}
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		CollectGraphsRecursive(Graph, TEXT(""), OutGraphs, 0);
	}
}

bool TryResolveTraceOptions(const TSharedPtr<FJsonObject>& Params, FCortexTraceOptions& OutOptions)
{
	if (!Params.IsValid())
	{
		return false;
	}

	Params->TryGetStringField(TEXT("graph_name"), OutOptions.GraphName);
	Params->TryGetStringField(TEXT("subgraph_path"), OutOptions.SubgraphPath);
	Params->TryGetNumberField(TEXT("max_depth"), OutOptions.MaxDepth);
	Params->TryGetBoolField(TEXT("include_edges"), OutOptions.bIncludeEdges);
	return true;
}

FCortexCommandResult ResolveGraphNode(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params,
	const FString& NodeId,
	FCortexResolvedGraphNode& OutResolved)
{
	FCortexCommandResult Error;
	FString GraphName;
	FString SubgraphPath;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("graph_name"), GraphName);
		Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
	}

	if (!GraphName.IsEmpty())
	{
		UEdGraph* Graph = FCortexGraphNodeOps::FindGraph(Blueprint, GraphName, Error);
		if (!Graph)
		{
			return Error;
		}
		if (!SubgraphPath.IsEmpty())
		{
			Graph = FCortexGraphNodeOps::ResolveSubgraph(Graph, SubgraphPath, Error);
			if (!Graph)
			{
				return Error;
			}
		}

		UEdGraphNode* Node = FCortexGraphNodeOps::FindNode(Graph, NodeId, Error);
		if (!Node)
		{
			return Error;
		}

		OutResolved.Graph = Graph;
		OutResolved.Node = Node;
		OutResolved.GraphName = GraphName.IsEmpty() ? Graph->GetName() : GraphName;
		OutResolved.SubgraphPath = SubgraphPath;
		return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
	}

	TArray<TPair<UEdGraph*, FString>> CandidateGraphs;
	GetAllCandidateGraphs(Blueprint, CandidateGraphs);

	for (const TPair<UEdGraph*, FString>& Entry : CandidateGraphs)
	{
		UEdGraph* Graph = Entry.Key;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->GetName() == NodeId)
			{
				OutResolved.Graph = Graph;
				OutResolved.Node = Node;
				OutResolved.GraphName = Graph->GetName();
				OutResolved.SubgraphPath = Entry.Value;
				return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
			}
		}
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::NodeNotFound,
		FString::Printf(TEXT("Node not found: %s"), *NodeId)
	);
}

TSharedRef<FJsonObject> MakeEdgeJson(const UEdGraphNode* SourceNode, const UEdGraphPin* SourcePin, const UEdGraphPin* TargetPin)
{
	TSharedRef<FJsonObject> EdgeJson = MakeShared<FJsonObject>();
	EdgeJson->SetStringField(TEXT("source_node"), SourceNode->GetName());
	EdgeJson->SetStringField(TEXT("source_pin"), SourcePin->PinName.ToString());
	EdgeJson->SetStringField(TEXT("target_node"), TargetPin->GetOwningNode()->GetName());
	EdgeJson->SetStringField(TEXT("target_pin"), TargetPin->PinName.ToString());
	return EdgeJson;
}

bool ShouldFollowPin(const UEdGraphPin* Pin, bool bExecOnly)
{
	if (!Pin || Pin->Direction != EGPD_Output)
	{
		return false;
	}

	const bool bIsExecPin = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	return bExecOnly ? bIsExecPin : !bIsExecPin;
}

void AddTraceResponseMetadata(TSharedRef<FJsonObject> NodeJson, const FString& GraphName, const FString& SubgraphPath)
{
	NodeJson->SetStringField(TEXT("graph_name"), GraphName);
	if (!SubgraphPath.IsEmpty())
	{
		NodeJson->SetStringField(TEXT("subgraph_path"), SubgraphPath);
	}
}

FCortexCommandResult RunTrace(const TSharedPtr<FJsonObject>& Params, bool bExecOnly)
{
	FString AssetPath;
	FString StartNodeId;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}
	if (!Params->TryGetStringField(TEXT("start_node_id"), StartNodeId))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: start_node_id"));
	}

	FCortexTraceOptions Options;
	TryResolveTraceOptions(Params, Options);
	Options.bExecOnly = bExecOnly;

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return LoadError;
	}

	FCortexResolvedGraphNode Start;
	FCortexCommandResult ResolveResult = ResolveGraphNode(Blueprint, Params, StartNodeId, Start);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	TArray<TSharedPtr<FJsonValue>> EdgesArray;
	TArray<TSharedPtr<FJsonValue>> CyclesArray;
	TSet<FString> VisitedNodeIds;
	TQueue<TPair<UEdGraphNode*, int32>> Pending;

	Pending.Enqueue(TPair<UEdGraphNode*, int32>(Start.Node, 0));
	VisitedNodeIds.Add(Start.Node->GetName());

	while (!Pending.IsEmpty())
	{
		TPair<UEdGraphNode*, int32> Current;
		Pending.Dequeue(Current);
		UEdGraphNode* CurrentNode = Current.Key;
		const int32 CurrentDepth = Current.Value;

		TSharedRef<FJsonObject> NodeJson = FCortexGraphNodeOps::SerializeNode(CurrentNode, true, true);
		AddTraceResponseMetadata(NodeJson, CurrentNode->GetGraph()->GetName(), TEXT(""));
		NodesArray.Add(MakeShared<FJsonValueObject>(NodeJson));

		if (CurrentDepth >= Options.MaxDepth)
		{
			continue;
		}

		for (UEdGraphPin* Pin : CurrentNode->Pins)
		{
			if (!ShouldFollowPin(Pin, bExecOnly))
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UEdGraphNode* NextNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!NextNode)
				{
					continue;
				}

				if (Options.bIncludeEdges)
				{
					EdgesArray.Add(MakeShared<FJsonValueObject>(MakeEdgeJson(CurrentNode, Pin, LinkedPin)));
				}

				if (VisitedNodeIds.Contains(NextNode->GetName()))
				{
					TSharedRef<FJsonObject> CycleJson = MakeShared<FJsonObject>();
					CycleJson->SetStringField(TEXT("node_id"), CurrentNode->GetName());
					CycleJson->SetStringField(TEXT("back_edge_to"), NextNode->GetName());
					CyclesArray.Add(MakeShared<FJsonValueObject>(CycleJson));
					continue;
				}

				VisitedNodeIds.Add(NextNode->GetName());
				Pending.Enqueue(TPair<UEdGraphNode*, int32>(NextNode, CurrentDepth + 1));
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("start_node_id"), StartNodeId);
	Data->SetArrayField(TEXT("nodes"), NodesArray);
	Data->SetNumberField(TEXT("node_count"), NodesArray.Num());
	if (Options.bIncludeEdges)
	{
		Data->SetArrayField(TEXT("edges"), EdgesArray);
	}
	if (CyclesArray.Num() > 0)
	{
		Data->SetArrayField(TEXT("cycles"), CyclesArray);
	}
	return FCortexCommandRouter::Success(Data);
}

bool DoesEventNameMatch(const UEdGraphNode* Node, const FString& EventName)
{
	const FString DisplayName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	return DisplayName.Equals(EventName, ESearchCase::IgnoreCase)
		|| DisplayName.Contains(EventName, ESearchCase::IgnoreCase)
		|| Node->GetName().Equals(EventName, ESearchCase::IgnoreCase)
		|| Node->GetName().Contains(EventName, ESearchCase::IgnoreCase);
}

bool IsSupportedEventHandlerNode(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return false;
	}

	if (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_CustomEvent>())
	{
		return true;
	}

	const FString ClassName = Node->GetClass()->GetName();
	return ClassName == TEXT("UK2Node_ActorBoundEvent") || ClassName == TEXT("UK2Node_ComponentBoundEvent");
}
}

FCortexCommandResult FCortexGraphTraceOps::TraceExec(const TSharedPtr<FJsonObject>& Params)
{
	return RunTrace(Params, true);
}

FCortexCommandResult FCortexGraphTraceOps::TraceDataflow(const TSharedPtr<FJsonObject>& Params)
{
	return RunTrace(Params, false);
}

FCortexCommandResult FCortexGraphTraceOps::GetSubgraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return LoadError;
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	FString SubgraphPath;
	Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);

	UEdGraph* Graph = FCortexGraphNodeOps::FindGraph(Blueprint, GraphName, LoadError);
	if (!Graph)
	{
		return LoadError;
	}
	if (!SubgraphPath.IsEmpty())
	{
		Graph = FCortexGraphNodeOps::ResolveSubgraph(Graph, SubgraphPath, LoadError);
		if (!Graph)
		{
			return LoadError;
		}
	}

	bool bIncludeEdges = false;
	Params->TryGetBoolField(TEXT("include_edges"), bIncludeEdges);
	bool bCompact = true;
	Params->TryGetBoolField(TEXT("compact"), bCompact);

	TSet<FString> RequestedNodeIds;
	FString SingleNodeId;
	const bool bHasSingleNodeId = Params->TryGetStringField(TEXT("node_id"), SingleNodeId) && !SingleNodeId.IsEmpty();
	const TArray<TSharedPtr<FJsonValue>>* NodeIdArray = nullptr;
	if (Params->TryGetArrayField(TEXT("node_ids"), NodeIdArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *NodeIdArray)
		{
			FString NodeId;
			if (Value.IsValid() && Value->TryGetString(NodeId))
			{
				RequestedNodeIds.Add(NodeId);
			}
		}
	}
	if (bHasSingleNodeId)
	{
		RequestedNodeIds.Add(SingleNodeId);
	}

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	TArray<TSharedPtr<FJsonValue>> EdgesArray;
	TSet<FString> IncludedNodeIds;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || (RequestedNodeIds.Num() > 0 && !RequestedNodeIds.Contains(Node->GetName())))
		{
			continue;
		}

		IncludedNodeIds.Add(Node->GetName());
		TSharedRef<FJsonObject> NodeJson = FCortexGraphNodeOps::SerializeNode(Node, true, bCompact);
		AddTraceResponseMetadata(NodeJson, Graph->GetName(), SubgraphPath);
		NodesArray.Add(MakeShared<FJsonValueObject>(NodeJson));
	}

	if (RequestedNodeIds.Num() > 0 && IncludedNodeIds.Num() != RequestedNodeIds.Num())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NodeNotFound,
			TEXT("One or more requested node_ids were not found in the resolved graph")
		);
	}

	if (bHasSingleNodeId && NodesArray.Num() == 1)
	{
		const TSharedPtr<FJsonObject>* SingleNode = nullptr;
		if (NodesArray[0].IsValid() && NodesArray[0]->TryGetObject(SingleNode) && SingleNode)
		{
			return FCortexCommandRouter::Success(*SingleNode);
		}
	}

	if (bIncludeEdges)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || !IncludedNodeIds.Contains(Node->GetName()))
			{
				continue;
			}

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNode())
					{
						continue;
					}
					if (IncludedNodeIds.Contains(LinkedPin->GetOwningNode()->GetName()))
					{
						EdgesArray.Add(MakeShared<FJsonValueObject>(MakeEdgeJson(Node, Pin, LinkedPin)));
					}
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("graph_name"), Graph->GetName());
	if (!SubgraphPath.IsEmpty())
	{
		Data->SetStringField(TEXT("subgraph_path"), SubgraphPath);
	}
	Data->SetArrayField(TEXT("nodes"), NodesArray);
	Data->SetNumberField(TEXT("node_count"), NodesArray.Num());
	if (bIncludeEdges)
	{
		Data->SetArrayField(TEXT("edges"), EdgesArray);
	}
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphTraceOps::ListEventHandlers(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return LoadError;
	}

	TArray<TPair<UEdGraph*, FString>> CandidateGraphs;
	GetAllCandidateGraphs(Blueprint, CandidateGraphs);

	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	for (const TPair<UEdGraph*, FString>& Entry : CandidateGraphs)
	{
		for (UEdGraphNode* Node : Entry.Key->Nodes)
		{
			if (!IsSupportedEventHandlerNode(Node))
			{
				continue;
			}

			TSharedRef<FJsonObject> NodeJson = FCortexGraphNodeOps::SerializeNode(Node, true, true);
			AddTraceResponseMetadata(NodeJson, Entry.Key->GetName(), Entry.Value);
			MatchesArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("nodes"), MatchesArray);
	Data->SetNumberField(TEXT("count"), MatchesArray.Num());
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphTraceOps::FindEventHandler(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString EventName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}
	if (!Params->TryGetStringField(TEXT("event_name"), EventName))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: event_name"));
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return LoadError;
	}

	TArray<TPair<UEdGraph*, FString>> CandidateGraphs;
	GetAllCandidateGraphs(Blueprint, CandidateGraphs);

	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	for (const TPair<UEdGraph*, FString>& Entry : CandidateGraphs)
	{
		for (UEdGraphNode* Node : Entry.Key->Nodes)
		{
			if (!IsSupportedEventHandlerNode(Node) || !DoesEventNameMatch(Node, EventName))
			{
				continue;
			}

			TSharedRef<FJsonObject> NodeJson = FCortexGraphNodeOps::SerializeNode(Node, true, true);
			AddTraceResponseMetadata(NodeJson, Entry.Key->GetName(), Entry.Value);
			MatchesArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("event_name"), EventName);
	Data->SetArrayField(TEXT("nodes"), MatchesArray);
	Data->SetNumberField(TEXT("count"), MatchesArray.Num());
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphTraceOps::FindFunctionCalls(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString FunctionName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: function_name"));
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return LoadError;
	}

	TArray<TPair<UEdGraph*, FString>> CandidateGraphs;
	GetAllCandidateGraphs(Blueprint, CandidateGraphs);

	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	for (const TPair<UEdGraph*, FString>& Entry : CandidateGraphs)
	{
		for (UEdGraphNode* Node : Entry.Key->Nodes)
		{
			const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
			if (!CallNode)
			{
				continue;
			}

			FString CandidateName;
			if (const UFunction* TargetFunction = CallNode->GetTargetFunction())
			{
				CandidateName = TargetFunction->GetName();
			}
			else
			{
				CandidateName = CallNode->FunctionReference.GetMemberName().ToString();
			}

			if (!CandidateName.Contains(FunctionName, ESearchCase::IgnoreCase))
			{
				continue;
			}

			TSharedRef<FJsonObject> NodeJson = FCortexGraphNodeOps::SerializeNode(Node, true, true);
			NodeJson->SetStringField(TEXT("function_name"), CandidateName);
			AddTraceResponseMetadata(NodeJson, Entry.Key->GetName(), Entry.Value);
			MatchesArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("function_name"), FunctionName);
	Data->SetArrayField(TEXT("nodes"), MatchesArray);
	Data->SetNumberField(TEXT("count"), MatchesArray.Num());
	return FCortexCommandRouter::Success(Data);
}
