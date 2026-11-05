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
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_Select.h"
#include "K2Node_Switch.h"
#include "K2Node_VariableGet.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPGraphExecReach.h"
#include "Operations/CortexBPGraphReachability.h"

namespace
{
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

	/** Counted as logic: enabled, and not a comment, knot, or function result. */
	bool IsFunctionalNode(const UEdGraphNode* Node)
	{
		return Node->IsNodeEnabled() && !IsCommentOrKnot(Node) && !Node->IsA<UK2Node_FunctionResult>();
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

	using CortexBPGraphReachability::FReachContext;
	using CortexBPGraphReachability::GetInnerGraph;
	using CortexBPGraphReachability::IsExpandableNode;
	using CortexBPGraphReachability::IsStructuralTunnel;

	bool HasLinkedExecOutput(const UEdGraphNode* Node)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				&& Pin->LinkedTo.Num() > 0)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Functional/conditional count of what a context reached. A macro or composite instance is replaced by the nodes its own
	 * context reached; an unresolved instance counts as the node itself, a recursive one adds nothing (its ancestor counts it).
	 */
	void CountContext(const FReachContext& Context, FLogicCounts& InOutCounts)
	{
		for (const UEdGraphNode* Node : Context.Reached)
		{
			if (!Node || (Context.IsRoot() && CortexBPGraphReachability::IsEntryNode(Node))
				|| IsStructuralTunnel(Node) || !IsFunctionalNode(Node))
			{
				continue;
			}

			if (IsExpandableNode(Node))
			{
				if (const FReachContext* Child = Context.FindChild(Node))
				{
					CountContext(*Child, InOutCounts);
					continue;
				}
				if (GetInnerGraph(Node))
				{
					continue;
				}
			}

			++InOutCounts.Functional;
			if (IsConditionalNode(Node))
			{
				++InOutCounts.Conditionals;
			}
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

	/**
	 * Distinct consumer nodes of an output pin. A boundary edge is translated instead of counted: a wire into a reached
	 * macro/composite continues to the consumers of the matching entry tunnel pin inside it, and a wire into an exit tunnel
	 * continues to the consumers of the matching output pin of the owning instance.
	 */
	void CollectConsumers(const FReachContext& Context, const UEdGraphPin* Output, TSet<const UEdGraphNode*>& OutConsumers)
	{
		for (const UEdGraphPin* Linked : Output->LinkedTo)
		{
			const UEdGraphNode* Consumer = Linked ? Linked->GetOwningNode() : nullptr;
			if (!Consumer)
			{
				continue;
			}

			if (IsStructuralTunnel(Consumer))
			{
				const UEdGraphPin* InstanceOutput = (Context.Parent && Context.Instance)
					? Context.Instance->FindPin(Linked->PinName, EGPD_Output) : nullptr;
				if (InstanceOutput)
				{
					CollectConsumers(*Context.Parent, InstanceOutput, OutConsumers);
				}
				continue;
			}

			if (const FReachContext* Child = Context.FindChild(Consumer))
			{
				for (const UEdGraphNode* InnerNode : Child->Graph->Nodes)
				{
					const UEdGraphPin* EntryPin = IsStructuralTunnel(InnerNode) ? InnerNode->FindPin(Linked->PinName, EGPD_Output) : nullptr;
					if (EntryPin)
					{
						CollectConsumers(*Child, EntryPin, OutConsumers);
					}
				}
				continue;
			}

			OutConsumers.Add(Consumer);
		}
	}

	/** Variable getters of the context and of every instance it expanded; fan-out is measured over the expanded consumers. */
	void MeasureGetters(const FReachContext& Context, FEntryMetrics& InOutMetrics)
	{
		for (const UEdGraphNode* Node : Context.Reached)
		{
			if (!Node || !Node->IsA<UK2Node_VariableGet>())
			{
				continue;
			}

			TSet<const UEdGraphNode*> Consumers;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output)
				{
					CollectConsumers(Context, Pin, Consumers);
				}
			}
			++InOutMetrics.VariableGets;
			InOutMetrics.MaxGetterFanout = FMath::Max(InOutMetrics.MaxGetterFanout, Consumers.Num());
		}
		for (const TPair<const UEdGraphNode*, TUniquePtr<FReachContext>>& Pair : Context.Children)
		{
			MeasureGetters(*Pair.Value, InOutMetrics);
		}
	}

	void MeasureEntry(const FReachContext& Root, FEntryMetrics& InOutMetrics)
	{
		CountContext(Root, InOutMetrics.Counts);
		MeasureGetters(Root, InOutMetrics);
	}

	/** Reads an optional strictly-positive integer threshold. Returns false and fills OutError when invalid. */
	bool ReadThreshold(const TSharedPtr<FJsonObject>& Thresholds, const TCHAR* Field, int32& InOutValue, FString& OutError)
	{
		const TSharedPtr<FJsonValue> FieldValue = Thresholds.IsValid() ? Thresholds->TryGetField(Field) : nullptr;
		if (!FieldValue.IsValid() || FieldValue->IsNull())
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

	/** Appends one {graph[, subgraph], count, node_ids} row; no-op for an empty node list. */
	void AddNodeRow(TArray<TSharedPtr<FJsonValue>>& Rows, int32& InOutTotal, const UEdGraph* Graph, const UEdGraph* Subgraph,
		const TArray<const UEdGraphNode*>& Nodes)
	{
		if (Nodes.IsEmpty())
		{
			return;
		}

		TArray<TSharedPtr<FJsonValue>> NodeIds;
		for (const UEdGraphNode* Node : Nodes)
		{
			if (NodeIds.Num() >= MaxUnreachableNodeIds)
			{
				break;
			}
			NodeIds.Add(StringValue(Node->GetName()));
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("graph"), Graph ? Graph->GetName() : FString());
		if (Subgraph)
		{
			Row->SetStringField(TEXT("subgraph"), Subgraph->GetName());
		}
		Row->SetNumberField(TEXT("count"), Nodes.Num());
		Row->SetArrayField(TEXT("node_ids"), NodeIds);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		InOutTotal += Nodes.Num();
	}

	/** One inner graph with the union of the nodes its instances' contexts reached, and the graph that holds the first instance. */
	struct FInnerGraphReach
	{
		const UEdGraph* Inner = nullptr;
		const UEdGraph* OuterGraph = nullptr;
		TSet<UEdGraphNode*> Reached;
	};

	void CollectInnerGraphs(const FReachContext& Context, TArray<FInnerGraphReach>& OutInnerGraphs)
	{
		for (const TPair<const UEdGraphNode*, TUniquePtr<FReachContext>>& Pair : Context.Children)
		{
			const FReachContext& Child = *Pair.Value;
			FInnerGraphReach* Existing = OutInnerGraphs.FindByPredicate(
				[&Child](const FInnerGraphReach& Candidate) { return Candidate.Inner == Child.Graph; });
			if (!Existing)
			{
				Existing = &OutInnerGraphs.AddDefaulted_GetRef();
				Existing->Inner = Child.Graph;
				Existing->OuterGraph = Pair.Key->GetGraph();
			}
			Existing->Reached.Append(Child.Reached);
			CollectInnerGraphs(Child, OutInnerGraphs);
		}
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

	FThresholds Thresholds;
	const TSharedPtr<FJsonValue> ThresholdsValue = Params->TryGetField(TEXT("thresholds"));
	if (ThresholdsValue.IsValid() && !ThresholdsValue->IsNull())
	{
		const TSharedPtr<FJsonObject>* ThresholdsPtr = nullptr;
		if (!ThresholdsValue->TryGetObject(ThresholdsPtr) || !ThresholdsPtr || !ThresholdsPtr->IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("thresholds must be an object"));
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ThresholdsPtr)->Values)
		{
			if (Pair.Key != TEXT("max_nodes_per_entry") && Pair.Key != TEXT("max_nodes_per_blueprint"))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(
					TEXT("Unknown thresholds key '%s'; allowed: max_nodes_per_entry, max_nodes_per_blueprint"), *Pair.Key));
			}
		}

		FString ThresholdError;
		if (!ReadThreshold(*ThresholdsPtr, TEXT("max_nodes_per_entry"), Thresholds.MaxNodesPerEntry, ThresholdError)
			|| !ReadThreshold(*ThresholdsPtr, TEXT("max_nodes_per_blueprint"), Thresholds.MaxNodesPerBlueprint, ThresholdError))
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

	return FCortexCommandRouter::Success(BuildGraphMetrics(Blueprint, AssetPath, Thresholds));
}

TSharedPtr<FJsonObject> FCortexBPGraphMetricsOps::BuildGraphMetrics(
	const UBlueprint* Blueprint, const FString& AssetPath, const FThresholds& Thresholds)
{
	const int32 MaxNodesPerEntry = Thresholds.MaxNodesPerEntry;
	const int32 MaxNodesPerBlueprint = Thresholds.MaxNodesPerBlueprint;

	// K2-derived schemas (including UWidgetGraphSchema) except animation and state machine graphs, whose
	// pose-flow networks have no event/exec semantics. The graph type is asked of the schema so no anim module is linked.
	// Macro graphs are not measured on their own; they count only where a macro instance expands them.
	TArray<const UEdGraph*> Graphs;
	const auto AddMeasurableGraph = [&Graphs](const UEdGraph* Graph)
	{
		if (Graph && Graph->Schema && Graph->Schema->IsChildOf(UEdGraphSchema_K2::StaticClass())
			&& !CortexBPGraphReachability::IsAnimationOrStateMachineGraph(Graph))
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
	// One merged context per graph over its live entries: the distinct union behind the Blueprint totals and the inner-graph findings.
	TMap<const UEdGraph*, TUniquePtr<FReachContext>> LiveRoots;
	TArray<TSharedPtr<FJsonValue>> UnreachableArray;
	TArray<TSharedPtr<FJsonValue>> DisabledArray;
	int32 TotalUnreachable = 0;
	int32 TotalDisabled = 0;

	for (const UEdGraph* Graph : Graphs)
	{
		TSet<UEdGraphNode*> GraphReachable;
		TArray<UEdGraphNode*> FunctionResults;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->IsA<UK2Node_FunctionResult>() && Node->IsNodeEnabled())
			{
				FunctionResults.Add(Node);
			}
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || !CortexBPGraphReachability::IsEntryNode(Node))
			{
				continue;
			}

			// A disabled entry seeds nothing: the compiler prunes it and everything only it reaches.
			FReachContext Root(Graph, nullptr, nullptr);
			Root.SeedEntry(Node);
			const UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(Node);
			if (FunctionEntry && Root.Reached.Contains(Node))
			{
				// A result with no exec wire from the entry is logic only for a pure function or a function whose single
				// result is simply unwired (its pure feeders then compute the return value). Any other exec-unreachable
				// result is dead code and is reported as unreachable together with its feeders.
				const bool bPure = (FunctionEntry->GetExtraFlags() & FUNC_BlueprintPure) != 0;
				const bool bSoleUnwiredResult = FunctionResults.Num() == 1 && !HasLinkedExecOutput(Node);
				if (bPure || bSoleUnwiredResult)
				{
					for (UEdGraphNode* ResultNode : FunctionResults)
					{
						Root.AddNode(ResultNode);
					}
				}
			}
			Root.ExpandDataClosure();
			GraphReachable.Append(Root.Reached);

			FEntryMetrics Metrics;
			Metrics.Graph = Graph;
			Metrics.Node = Node;
			Metrics.Kind = ClassifyEntry(Node);
			Metrics.Name = GetEntryName(Graph, Node, Metrics.Kind);
			Metrics.bLive = CortexBPGraphReachability::IsLiveEntry(Node);
			if (Metrics.bLive)
			{
				MeasureEntry(Root, Metrics);
				TUniquePtr<FReachContext>& Merged = LiveRoots.FindOrAdd(Graph);
				if (!Merged)
				{
					Merged = MakeUnique<FReachContext>(Graph, nullptr, nullptr);
				}
				Merged->MergeFrom(Root);
			}
			Entries.Add(MoveTemp(Metrics));
		}

		TArray<const UEdGraphNode*> UnreachableNodes;
		TArray<const UEdGraphNode*> DisabledNodes;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || IsCommentOrKnot(Node))
			{
				continue;
			}
			if (!Node->IsNodeEnabled())
			{
				DisabledNodes.Add(Node);
			}
			else if (!CortexBPGraphReachability::IsEntryNode(Node) && !GraphReachable.Contains(const_cast<UEdGraphNode*>(Node)))
			{
				UnreachableNodes.Add(Node);
			}
		}
		AddNodeRow(UnreachableArray, TotalUnreachable, Graph, nullptr, UnreachableNodes);
		AddNodeRow(DisabledArray, TotalDisabled, Graph, nullptr, DisabledNodes);
	}

	// Macro and collapsed-graph contents: inner nodes that no entry tunnel reaches (or that are disabled) are reported
	// once per inner graph, with the owning graph and the inner graph's name.
	{
		TArray<FInnerGraphReach> InnerGraphs;
		for (const TPair<const UEdGraph*, TUniquePtr<FReachContext>>& Pair : LiveRoots)
		{
			CollectInnerGraphs(*Pair.Value, InnerGraphs);
		}
		InnerGraphs.Sort([](const FInnerGraphReach& A, const FInnerGraphReach& B)
		{
			return A.Inner->GetPathName() < B.Inner->GetPathName();
		});

		for (const FInnerGraphReach& InnerReach : InnerGraphs)
		{
			// Macros of other assets are counted by reachability but are not this Blueprint's findings.
			if (InnerReach.Inner->GetTypedOuter<UBlueprint>() != Blueprint)
			{
				continue;
			}

			TArray<const UEdGraphNode*> UnreachableNodes;
			TArray<const UEdGraphNode*> DisabledNodes;
			for (const UEdGraphNode* InnerNode : InnerReach.Inner->Nodes)
			{
				if (!InnerNode || IsCommentOrKnot(InnerNode) || IsStructuralTunnel(InnerNode))
				{
					continue;
				}
				if (!InnerNode->IsNodeEnabled())
				{
					DisabledNodes.Add(InnerNode);
				}
				else if (!InnerReach.Reached.Contains(const_cast<UEdGraphNode*>(InnerNode)))
				{
					UnreachableNodes.Add(InnerNode);
				}
			}
			AddNodeRow(UnreachableArray, TotalUnreachable, InnerReach.OuterGraph, InnerReach.Inner, UnreachableNodes);
			AddNodeRow(DisabledArray, TotalDisabled, InnerReach.OuterGraph, InnerReach.Inner, DisabledNodes);
		}
	}

	bool bHasTick = false;
	int32 TotalFunctional = 0;
	for (const TPair<const UEdGraph*, TUniquePtr<FReachContext>>& Pair : LiveRoots)
	{
		FLogicCounts Distinct;
		CountContext(*Pair.Value, Distinct);
		TotalFunctional += Distinct.Functional;
	}
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
	Totals->SetNumberField(TEXT("disabled_nodes"), TotalDisabled);
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
	Data->SetArrayField(TEXT("disabled"), DisabledArray);
	Data->SetArrayField(TEXT("ghost_entries"), GhostArray);
	Data->SetObjectField(TEXT("thresholds"), ThresholdsOut);
	Data->SetArrayField(TEXT("violations"), ViolationArray);
	return Data;
}
