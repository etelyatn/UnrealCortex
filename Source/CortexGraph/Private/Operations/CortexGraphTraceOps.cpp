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

/** Params reduced to the graph identity selectors (graph_guid, graph_path), for commands that scope by identity only. */
TSharedPtr<FJsonObject> IdentityScopeParams(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Scope = MakeShared<FJsonObject>();
	FString Value;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("graph_guid"), Value))
	{
		Scope->SetStringField(TEXT("graph_guid"), Value);
	}
	if (Params.IsValid() && Params->TryGetStringField(TEXT("graph_path"), Value))
	{
		Scope->SetStringField(TEXT("graph_path"), Value);
	}
	return Scope;
}

/**
 * Params for the handler and call reads: without graph_guid/graph_path they stay unscoped (graph_name and subgraph_path were
 * never a scope for these commands); with an identity, graph_name and subgraph_path ride along so the shared resolver validates
 * that they agree with it.
 */
TSharedPtr<FJsonObject> HandlerScopeParams(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Scope = IdentityScopeParams(Params);
	if (!FCortexGraphTree::HasGraphSelector(Scope))
	{
		return Scope;
	}
	FString Value;
	if (Params->TryGetStringField(TEXT("graph_name"), Value))
	{
		Scope->SetStringField(TEXT("graph_name"), Value);
	}
	if (Params->TryGetStringField(TEXT("subgraph_path"), Value))
	{
		Scope->SetStringField(TEXT("subgraph_path"), Value);
	}
	return Scope;
}

/**
 * Params reduced to the selectors a trace honours: graph_guid, graph_path and graph_name, with subgraph_path only alongside
 * one of them (a bare subgraph_path never narrowed a trace).
 */
TSharedPtr<FJsonObject> TraceSelectorParams(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Selector = IdentityScopeParams(Params);
	FString Value;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("graph_name"), Value))
	{
		Selector->SetStringField(TEXT("graph_name"), Value);
	}
	if (FCortexGraphTree::HasGraphSelector(Selector) && Params->TryGetStringField(TEXT("subgraph_path"), Value))
	{
		Selector->SetStringField(TEXT("subgraph_path"), Value);
	}
	return Selector;
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
	const TSharedPtr<FJsonObject> Selector = TraceSelectorParams(Params);
	if (FCortexGraphTree::HasGraphSelector(Selector))
	{
		FCortexCommandResult Error;
		UEdGraph* Graph = nullptr;
		FCortexGraphTreeEntry Target;
		if (!FCortexGraphTree::ResolveReadTarget(Blueprint, Selector, Graph, &Target, Error))
		{
			return Error;
		}

		UEdGraphNode* Node = FCortexGraphNodeOps::FindNode(Graph, NodeId, Error);
		if (!Node)
		{
			return Error;
		}

		OutResolved.Graph = Graph;
		OutResolved.Node = Node;
		OutResolved.GraphName = Graph->GetName();
		OutResolved.SubgraphPath = Target.SubgraphPath;
		return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
	}

	// No graph named: the first node of that name in the tree, nested graphs included.
	TArray<FCortexGraphTreeEntry> Tree;
	FCortexGraphTree::Collect(Blueprint, Tree);
	for (const FCortexGraphTreeEntry& Entry : Tree)
	{
		for (UEdGraphNode* Node : Entry.Graph->Nodes)
		{
			if (Node && Node->GetName() == NodeId)
			{
				OutResolved.Graph = Entry.Graph;
				OutResolved.Node = Node;
				OutResolved.GraphName = Entry.Graph->GetName();
				OutResolved.SubgraphPath = Entry.SubgraphPath;
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

void AddGraphGuid(TSharedRef<FJsonObject> NodeJson, const UEdGraph* Graph)
{
	NodeJson->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
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
		AddGraphGuid(NodeJson, CurrentNode->GetGraph());
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

	UEdGraph* Graph = nullptr;
	FCortexGraphTreeEntry Target;
	if (!FCortexGraphTree::ResolveReadTarget(Blueprint, Params, Graph, &Target, LoadError))
	{
		return LoadError;
	}

	// A graph addressed by identity echoes graph_guid/graph_kind/graph_path; name-addressed callers keep their existing response.
	FString IdentityGuid;
	FString IdentityPath;
	Params->TryGetStringField(TEXT("graph_guid"), IdentityGuid);
	Params->TryGetStringField(TEXT("graph_path"), IdentityPath);
	const bool bByIdentity = !IdentityGuid.IsEmpty() || !IdentityPath.IsEmpty();
	FString SubgraphPath;
	if (bByIdentity)
	{
		SubgraphPath = Target.SubgraphPath;
	}
	else
	{
		Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
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
			if (bByIdentity)
			{
				(*SingleNode)->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
				(*SingleNode)->SetStringField(TEXT("graph_kind"), Target.Kind);
			}
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
	if (bByIdentity)
	{
		Data->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
		Data->SetStringField(TEXT("graph_kind"), Target.Kind);
		Data->SetStringField(TEXT("graph_path"), Target.Path);
	}
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

	TArray<FCortexGraphTreeEntry> CandidateGraphs;
	FCortexCommandResult ScopeError;
	if (!FCortexGraphTree::ResolveReadScope(Blueprint, HandlerScopeParams(Params), CandidateGraphs, ScopeError))
	{
		return ScopeError;
	}

	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	for (const FCortexGraphTreeEntry& Entry : CandidateGraphs)
	{
		for (UEdGraphNode* Node : Entry.Graph->Nodes)
		{
			if (!IsSupportedEventHandlerNode(Node))
			{
				continue;
			}

			TSharedRef<FJsonObject> NodeJson = FCortexGraphNodeOps::SerializeNode(Node, true, true);
			AddTraceResponseMetadata(NodeJson, Entry.Graph->GetName(), Entry.SubgraphPath);
			AddGraphGuid(NodeJson, Entry.Graph);
			MatchesArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("nodes"), MatchesArray);
	Data->SetNumberField(TEXT("count"), MatchesArray.Num());
	FCortexGraphTree::AppendTruncation(CandidateGraphs, Data);
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

	TArray<FCortexGraphTreeEntry> CandidateGraphs;
	FCortexCommandResult ScopeError;
	if (!FCortexGraphTree::ResolveReadScope(Blueprint, HandlerScopeParams(Params), CandidateGraphs, ScopeError))
	{
		return ScopeError;
	}

	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	for (const FCortexGraphTreeEntry& Entry : CandidateGraphs)
	{
		for (UEdGraphNode* Node : Entry.Graph->Nodes)
		{
			if (!IsSupportedEventHandlerNode(Node) || !DoesEventNameMatch(Node, EventName))
			{
				continue;
			}

			TSharedRef<FJsonObject> NodeJson = FCortexGraphNodeOps::SerializeNode(Node, true, true);
			AddTraceResponseMetadata(NodeJson, Entry.Graph->GetName(), Entry.SubgraphPath);
			AddGraphGuid(NodeJson, Entry.Graph);
			MatchesArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("event_name"), EventName);
	Data->SetArrayField(TEXT("nodes"), MatchesArray);
	Data->SetNumberField(TEXT("count"), MatchesArray.Num());
	FCortexGraphTree::AppendTruncation(CandidateGraphs, Data);
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

	TArray<FCortexGraphTreeEntry> CandidateGraphs;
	FCortexCommandResult ScopeError;
	if (!FCortexGraphTree::ResolveReadScope(Blueprint, HandlerScopeParams(Params), CandidateGraphs, ScopeError))
	{
		return ScopeError;
	}

	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	for (const FCortexGraphTreeEntry& Entry : CandidateGraphs)
	{
		for (UEdGraphNode* Node : Entry.Graph->Nodes)
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
			AddTraceResponseMetadata(NodeJson, Entry.Graph->GetName(), Entry.SubgraphPath);
			AddGraphGuid(NodeJson, Entry.Graph);
			MatchesArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("function_name"), FunctionName);
	Data->SetArrayField(TEXT("nodes"), MatchesArray);
	Data->SetNumberField(TEXT("count"), MatchesArray.Num());
	FCortexGraphTree::AppendTruncation(CandidateGraphs, Data);
	return FCortexCommandRouter::Success(Data);
}
