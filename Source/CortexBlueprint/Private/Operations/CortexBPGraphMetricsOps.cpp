#include "Operations/CortexBPGraphMetricsOps.h"

#include "CortexBlueprintModule.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Select.h"
#include "K2Node_Switch.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableGet.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPGraphReachability.h"

namespace
{
	constexpr int32 DefaultMaxNodesPerEntry = 10;
	constexpr int32 DefaultMaxNodesPerBlueprint = 30;
	constexpr int32 MaxUnreachableNodeIds = 50;

	struct FLogicCounts
	{
		int32 Functional = 0;
		int32 Conditionals = 0;
	};

	enum class EEntryKind : uint8
	{
		Event,
		Function,
		Input,
		CustomEvent,
		BoundEvent
	};

	struct FEntryMetrics
	{
		const UEdGraph* Graph = nullptr;
		const UEdGraphNode* Node = nullptr;
		FString Name;
		EEntryKind Kind = EEntryKind::Event;
		bool bLive = false;
		FLogicCounts Counts;
		int32 VariableGets = 0;
		int32 MaxGetterFanout = 0;
	};

	const TCHAR* KindToString(const EEntryKind Kind)
	{
		switch (Kind)
		{
		case EEntryKind::Event:
			return TEXT("event");
		case EEntryKind::Function:
			return TEXT("function");
		case EEntryKind::Input:
			return TEXT("input");
		case EEntryKind::CustomEvent:
			return TEXT("custom_event");
		case EEntryKind::BoundEvent:
			return TEXT("bound_event");
		}
		return TEXT("event");
	}

	bool IsCommentOrKnot(const UEdGraphNode* Node)
	{
		return Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>();
	}

	/** Counted as logic: not a comment, knot, or function result. */
	bool IsFunctionalNode(const UEdGraphNode* Node)
	{
		return !IsCommentOrKnot(Node) && !Node->IsA<UK2Node_FunctionResult>();
	}

	bool IsConditionalNode(const UEdGraphNode* Node)
	{
		if (Node->IsA<UK2Node_IfThenElse>() || Node->IsA<UK2Node_Switch>() || Node->IsA<UK2Node_Select>())
		{
			return true;
		}
		const UK2Node_DynamicCast* DynamicCast = Cast<UK2Node_DynamicCast>(Node);
		return DynamicCast && !DynamicCast->IsNodePure();
	}

	const UEdGraph* GetInnerGraph(const UEdGraphNode* Node)
	{
		if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			return Macro->GetMacroGraph();
		}
		if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
		{
			return Composite->BoundGraph;
		}
		return nullptr;
	}

	bool IsExpandableNode(const UEdGraphNode* Node)
	{
		return Node->IsA<UK2Node_MacroInstance>() || Node->IsA<UK2Node_Composite>();
	}

	void AddNodeCounts(const UEdGraphNode* Node, TSet<const UEdGraph*>& GraphsInProgress, FLogicCounts& InOutCounts);

	/** Functional count of every node of an inner graph (macro or collapsed), recursing into nested inner graphs. */
	void CountInnerGraph(const UEdGraph* Graph, TSet<const UEdGraph*>& GraphsInProgress, FLogicCounts& InOutCounts)
	{
		if (!Graph || GraphsInProgress.Contains(Graph))
		{
			return;
		}

		GraphsInProgress.Add(Graph);
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || (Node->IsA<UK2Node_Tunnel>() && !IsExpandableNode(Node)))
			{
				continue;
			}
			AddNodeCounts(Node, GraphsInProgress, InOutCounts);
		}
		GraphsInProgress.Remove(Graph);
	}

	/** Adds one node's contribution: macro/composite nodes are replaced by their inner graph's counts. */
	void AddNodeCounts(const UEdGraphNode* Node, TSet<const UEdGraph*>& GraphsInProgress, FLogicCounts& InOutCounts)
	{
		if (!IsFunctionalNode(Node))
		{
			return;
		}

		if (IsExpandableNode(Node))
		{
			CountInnerGraph(GetInnerGraph(Node), GraphsInProgress, InOutCounts);
			return;
		}

		++InOutCounts.Functional;
		if (IsConditionalNode(Node))
		{
			++InOutCounts.Conditionals;
		}
	}

	EEntryKind ClassifyEntry(const UEdGraphNode* Node)
	{
		if (Node->IsA<UK2Node_FunctionEntry>())
		{
			return EEntryKind::Function;
		}
		if (Node->IsA<UK2Node_ComponentBoundEvent>())
		{
			return EEntryKind::BoundEvent;
		}
		if (Node->IsA<UK2Node_CustomEvent>())
		{
			return EEntryKind::CustomEvent;
		}
		if (Node->IsA<UK2Node_Event>())
		{
			return EEntryKind::Event;
		}
		return EEntryKind::Input;
	}

	FString GetEntryName(const UEdGraph* Graph, const UEdGraphNode* Node, const EEntryKind Kind)
	{
		switch (Kind)
		{
		case EEntryKind::Function:
			return Graph->GetName();
		case EEntryKind::BoundEvent:
		{
			const UK2Node_ComponentBoundEvent* Bound = CastChecked<UK2Node_ComponentBoundEvent>(Node);
			return FString::Printf(TEXT("%s.%s"),
				*Bound->ComponentPropertyName.ToString(), *Bound->DelegatePropertyName.ToString());
		}
		case EEntryKind::CustomEvent:
			return CastChecked<UK2Node_CustomEvent>(Node)->CustomFunctionName.ToString();
		case EEntryKind::Event:
		{
			const FName MemberName = CastChecked<UK2Node_Event>(Node)->EventReference.GetMemberName();
			if (!MemberName.IsNone())
			{
				return MemberName.ToString();
			}
			break;
		}
		case EEntryKind::Input:
			break;
		}
		return Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	}

	void MeasureEntry(const TSet<UEdGraphNode*>& Reachable, FEntryMetrics& InOutMetrics)
	{
		TSet<const UEdGraph*> GraphsInProgress;
		for (const UEdGraphNode* Node : Reachable)
		{
			if (!Node || Node == InOutMetrics.Node || CortexBPGraphReachability::IsEntryNode(Node))
			{
				continue;
			}

			AddNodeCounts(Node, GraphsInProgress, InOutMetrics.Counts);

			if (Node->IsA<UK2Node_VariableGet>())
			{
				TSet<const UEdGraphNode*> Consumers;
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output)
					{
						continue;
					}
					for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode())
						{
							Consumers.Add(LinkedPin->GetOwningNode());
						}
					}
				}
				++InOutMetrics.VariableGets;
				InOutMetrics.MaxGetterFanout = FMath::Max(InOutMetrics.MaxGetterFanout, Consumers.Num());
			}
		}
	}

	/** Reads an optional strictly-positive integer threshold. Returns false and fills OutError when invalid. */
	bool ReadThreshold(const TSharedPtr<FJsonObject>& Thresholds, const TCHAR* Field, int32& InOutValue, FString& OutError)
	{
		if (!Thresholds.IsValid() || !Thresholds->HasField(Field))
		{
			return true;
		}

		double Value = 0.0;
		if (!Thresholds->TryGetNumberField(Field, Value)
			|| Value < 1.0 || Value > static_cast<double>(MAX_int32) || FMath::FloorToDouble(Value) != Value)
		{
			OutError = FString::Printf(TEXT("thresholds.%s must be a positive integer"), Field);
			return false;
		}
		InOutValue = static_cast<int32>(Value);
		return true;
	}

	TSharedRef<FJsonValueString> StringValue(const FString& Value)
	{
		return MakeShared<FJsonValueString>(Value);
	}
}

FCortexCommandResult FCortexBPGraphMetricsOps::GraphMetrics(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path"));
	}

	int32 MaxNodesPerEntry = DefaultMaxNodesPerEntry;
	int32 MaxNodesPerBlueprint = DefaultMaxNodesPerBlueprint;
	if (Params->HasField(TEXT("thresholds")))
	{
		const TSharedPtr<FJsonObject>* ThresholdsPtr = nullptr;
		if (!Params->TryGetObjectField(TEXT("thresholds"), ThresholdsPtr) || !ThresholdsPtr || !ThresholdsPtr->IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("thresholds must be an object"));
		}

		FString ThresholdError;
		if (!ReadThreshold(*ThresholdsPtr, TEXT("max_nodes_per_entry"), MaxNodesPerEntry, ThresholdError)
			|| !ReadThreshold(*ThresholdsPtr, TEXT("max_nodes_per_blueprint"), MaxNodesPerBlueprint, ThresholdError))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ThresholdError);
		}
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	// K2-derived schemas (including UWidgetGraphSchema) except animation and state machine graphs, whose
	// pose-flow networks have no event/exec semantics. The graph type is asked of the schema so no anim module is linked.
	// Macro graphs are not measured on their own; they count only where a macro instance expands them.
	TArray<const UEdGraph*> Graphs;
	const auto AddMeasurableGraph = [&Graphs](const UEdGraph* Graph)
	{
		if (!Graph || !Graph->Schema || !Graph->Schema->IsChildOf(UEdGraphSchema_K2::StaticClass()))
		{
			return;
		}
		const EGraphType GraphType = GetDefault<UEdGraphSchema>(Graph->Schema)->GetGraphType(Graph);
		if (GraphType != GT_Animation && GraphType != GT_StateMachine)
		{
			Graphs.Add(Graph);
		}
	};
	for (const UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		AddMeasurableGraph(Graph);
	}
	for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		AddMeasurableGraph(Graph);
	}
	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		for (const UEdGraph* Graph : Interface.Graphs)
		{
			AddMeasurableGraph(Graph);
		}
	}

	TArray<FEntryMetrics> Entries;
	TSet<UEdGraphNode*> LiveUnion;
	TArray<TSharedPtr<FJsonValue>> UnreachableArray;
	int32 TotalUnreachable = 0;

	for (const UEdGraph* Graph : Graphs)
	{
		TSet<UEdGraphNode*> GraphReachable;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || !CortexBPGraphReachability::IsEntryNode(Node))
			{
				continue;
			}

			TSet<UEdGraphNode*> Reachable;
			CortexBPGraphReachability::GatherReachableFromEntry(Node, Reachable);
			if (Node->IsA<UK2Node_FunctionEntry>())
			{
				// A function result may be fed by pure nodes with no exec wire from the entry; those nodes are logic too.
				for (UEdGraphNode* ResultNode : Graph->Nodes)
				{
					if (ResultNode && ResultNode->IsA<UK2Node_FunctionResult>())
					{
						Reachable.Add(ResultNode);
					}
				}
				CortexBPGraphReachability::ExpandBackwardDataClosure(Reachable);
			}
			GraphReachable.Append(Reachable);

			FEntryMetrics Metrics;
			Metrics.Graph = Graph;
			Metrics.Node = Node;
			Metrics.Kind = ClassifyEntry(Node);
			Metrics.Name = GetEntryName(Graph, Node, Metrics.Kind);
			Metrics.bLive = CortexBPGraphReachability::IsLiveEntry(Node);
			if (Metrics.bLive)
			{
				MeasureEntry(Reachable, Metrics);
				for (UEdGraphNode* Reached : Reachable)
				{
					if (Reached && !CortexBPGraphReachability::IsEntryNode(Reached))
					{
						LiveUnion.Add(Reached);
					}
				}
			}
			Entries.Add(MoveTemp(Metrics));
		}

		int32 UnreachableInGraph = 0;
		TArray<TSharedPtr<FJsonValue>> UnreachableIds;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || IsCommentOrKnot(Node) || CortexBPGraphReachability::IsEntryNode(Node)
				|| GraphReachable.Contains(const_cast<UEdGraphNode*>(Node)))
			{
				continue;
			}
			++UnreachableInGraph;
			if (UnreachableIds.Num() < MaxUnreachableNodeIds)
			{
				UnreachableIds.Add(StringValue(Node->GetName()));
			}
		}

		if (UnreachableInGraph > 0)
		{
			TSharedPtr<FJsonObject> UnreachableObject = MakeShared<FJsonObject>();
			UnreachableObject->SetStringField(TEXT("graph"), Graph->GetName());
			UnreachableObject->SetNumberField(TEXT("count"), UnreachableInGraph);
			UnreachableObject->SetArrayField(TEXT("node_ids"), UnreachableIds);
			UnreachableArray.Add(MakeShared<FJsonValueObject>(UnreachableObject));
			TotalUnreachable += UnreachableInGraph;
		}
	}

	bool bHasTick = false;
	const int32 TotalFunctional = [&LiveUnion]()
	{
		FLogicCounts Distinct;
		TSet<const UEdGraph*> GraphsInProgress;
		for (const UEdGraphNode* Node : LiveUnion)
		{
			AddNodeCounts(Node, GraphsInProgress, Distinct);
		}
		return Distinct.Functional;
	}();
	int32 GhostCount = 0;
	TArray<TSharedPtr<FJsonValue>> EntryArray;
	TArray<TSharedPtr<FJsonValue>> GhostArray;
	TArray<TSharedPtr<FJsonValue>> ViolationArray;

	for (const FEntryMetrics& Metrics : Entries)
	{
		TSharedPtr<FJsonObject> EntryObject = MakeShared<FJsonObject>();
		EntryObject->SetStringField(TEXT("graph"), Metrics.Graph->GetName());
		EntryObject->SetStringField(TEXT("name"), Metrics.Name);
		EntryObject->SetStringField(TEXT("kind"), KindToString(Metrics.Kind));
		EntryObject->SetStringField(TEXT("node_id"), Metrics.Node->GetName());
		EntryObject->SetBoolField(TEXT("live"), Metrics.bLive);
		EntryObject->SetNumberField(TEXT("functional_nodes"), Metrics.Counts.Functional);
		EntryObject->SetNumberField(TEXT("conditionals"), Metrics.Counts.Conditionals);
		EntryObject->SetNumberField(TEXT("variable_gets"), Metrics.VariableGets);
		EntryObject->SetNumberField(TEXT("max_getter_fanout"), Metrics.MaxGetterFanout);
		EntryArray.Add(MakeShared<FJsonValueObject>(EntryObject));

		if (!Metrics.bLive)
		{
			++GhostCount;
			TSharedPtr<FJsonObject> GhostObject = MakeShared<FJsonObject>();
			GhostObject->SetStringField(TEXT("graph"), Metrics.Graph->GetName());
			GhostObject->SetStringField(TEXT("name"), Metrics.Name);
			GhostObject->SetStringField(TEXT("node_id"), Metrics.Node->GetName());
			GhostArray.Add(MakeShared<FJsonValueObject>(GhostObject));
			continue;
		}

		if (Metrics.Kind == EEntryKind::Event && (Metrics.Name == TEXT("ReceiveTick") || Metrics.Name == TEXT("Tick")))
		{
			bHasTick = true;
		}

		if (Metrics.Counts.Functional > MaxNodesPerEntry)
		{
			TSharedPtr<FJsonObject> Violation = MakeShared<FJsonObject>();
			Violation->SetStringField(TEXT("scope"), TEXT("entry"));
			Violation->SetStringField(TEXT("graph"), Metrics.Graph->GetName());
			Violation->SetStringField(TEXT("name"), Metrics.Name);
			Violation->SetNumberField(TEXT("functional_nodes"), Metrics.Counts.Functional);
			Violation->SetNumberField(TEXT("limit"), MaxNodesPerEntry);
			ViolationArray.Add(MakeShared<FJsonValueObject>(Violation));
		}
	}

	if (TotalFunctional > MaxNodesPerBlueprint)
	{
		TSharedPtr<FJsonObject> Violation = MakeShared<FJsonObject>();
		Violation->SetStringField(TEXT("scope"), TEXT("blueprint"));
		Violation->SetNumberField(TEXT("functional_nodes"), TotalFunctional);
		Violation->SetNumberField(TEXT("limit"), MaxNodesPerBlueprint);
		ViolationArray.Add(MakeShared<FJsonValueObject>(Violation));
	}

	TSharedPtr<FJsonObject> Totals = MakeShared<FJsonObject>();
	Totals->SetNumberField(TEXT("functional_nodes"), TotalFunctional);
	Totals->SetNumberField(TEXT("unreachable_nodes"), TotalUnreachable);
	Totals->SetNumberField(TEXT("ghost_entries"), GhostCount);
	Totals->SetNumberField(TEXT("entries"), Entries.Num());

	TSharedPtr<FJsonObject> ThresholdsOut = MakeShared<FJsonObject>();
	ThresholdsOut->SetNumberField(TEXT("max_nodes_per_entry"), MaxNodesPerEntry);
	ThresholdsOut->SetNumberField(TEXT("max_nodes_per_blueprint"), MaxNodesPerBlueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("has_tick"), bHasTick);
	Data->SetObjectField(TEXT("totals"), Totals);
	Data->SetArrayField(TEXT("entries"), EntryArray);
	Data->SetArrayField(TEXT("unreachable"), UnreachableArray);
	Data->SetArrayField(TEXT("ghost_entries"), GhostArray);
	Data->SetObjectField(TEXT("thresholds"), ThresholdsOut);
	Data->SetArrayField(TEXT("violations"), ViolationArray);
	return FCortexCommandRouter::Success(Data);
}
