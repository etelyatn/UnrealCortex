#include "Operations/CortexGraphNodeOps.h"
#include "CortexGraphModule.h"
#include "CortexSerializer.h"
#include "CortexGraphLayoutOps.h"
#include "CortexBatchScope.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableSet.h"
#include "K2Node_VariableGet.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Self.h"
#include "K2Node_Knot.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Timeline.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Composite.h"
#include "K2Node_Tunnel.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "UObject/UnrealType.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/LevelScriptBlueprint.h"

UBlueprint* FCortexGraphNodeOps::LoadBlueprint(const FString& AssetPath, FCortexCommandResult& OutError)
{
	// Level Script Blueprint: synthetic path __level_bp__:/Game/Maps/MapName
	static const FString LevelBPPrefix = TEXT("__level_bp__:");
	if (AssetPath.StartsWith(LevelBPPrefix))
	{
		const FString MapPath = AssetPath.Mid(LevelBPPrefix.Len());

		UWorld* World = nullptr;
		if (GEditor)
		{
			UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
			if (EditorWorld && EditorWorld->GetOutermost()->GetName() == MapPath)
			{
				World = EditorWorld;
			}
		}

		if (!World)
		{
			UPackage* MapPackage = LoadPackage(nullptr, *MapPath, LOAD_None);
			if (!MapPackage)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::AssetNotFound,
					FString::Printf(TEXT("Map package not found: %s"), *MapPath)
				);
				return nullptr;
			}
			World = UWorld::FindWorldInPackage(MapPackage);
		}

		if (!World)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::AssetNotFound,
				FString::Printf(TEXT("No world found in map package: %s"), *MapPath)
			);
			return nullptr;
		}

		ULevelScriptBlueprint* LSB = World->PersistentLevel->GetLevelScriptBlueprint(/*bDontCreate=*/false);
		if (!LSB)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::AssetNotFound,
				FString::Printf(TEXT("Failed to get Level Script Blueprint for: %s"), *MapPath)
			);
			return nullptr;
		}

		return LSB;
	}

	// Check if package exists before LoadObject to avoid SkipPackage warnings
	FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath)
		);
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (Blueprint == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath)
		);
	}
	return Blueprint;
}

UEdGraph* FCortexGraphNodeOps::FindGraph(UBlueprint* Blueprint, const FString& GraphName, FCortexCommandResult& OutError)
{
	FString TargetName = GraphName.IsEmpty() ? TEXT("EventGraph") : GraphName;

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName() == TargetName)
		{
			return Graph;
		}
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == TargetName)
		{
			return Graph;
		}
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::GraphNotFound,
		FString::Printf(TEXT("Graph not found: %s"), *TargetName)
	);
	return nullptr;
}

UEdGraphNode* FCortexGraphNodeOps::FindNode(UEdGraph* Graph, const FString& NodeId, FCortexCommandResult& OutError)
{
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->GetName() == NodeId)
		{
			return Node;
		}
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::NodeNotFound,
		FString::Printf(TEXT("Node not found: %s"), *NodeId)
	);
	return nullptr;
}

UEdGraphPin* FCortexGraphNodeOps::FindPin(UEdGraphNode* Node, const FString& PinName, FCortexCommandResult& OutError)
{
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString() == PinName)
		{
			return Pin;
		}
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::PinNotFound,
		FString::Printf(TEXT("Pin not found: %s on node %s"), *PinName, *Node->GetName())
	);
	return nullptr;
}

UEdGraph* FCortexGraphNodeOps::ResolveSubgraph(UEdGraph* RootGraph, const FString& SubgraphPath, FCortexCommandResult& OutError)
{
	if (SubgraphPath.IsEmpty())
	{
		return RootGraph;
	}

	TArray<FString> Segments;
	SubgraphPath.ParseIntoArray(Segments, TEXT("."), true);

	if (Segments.Num() > MaxSubgraphDepth)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::SubgraphDepthExceeded,
			FString::Printf(TEXT("Subgraph path exceeds max depth of %d: %s"), MaxSubgraphDepth, *SubgraphPath)
		);
		return nullptr;
	}

	UEdGraph* CurrentGraph = RootGraph;

	for (const FString& Segment : Segments)
	{
		bool bFound = false;
		for (UEdGraphNode* Node : CurrentGraph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node);
			if (CompositeNode && CompositeNode->BoundGraph && CompositeNode->BoundGraph->GetName() == Segment)
			{
				CurrentGraph = CompositeNode->BoundGraph;
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::SubgraphNotFound,
				FString::Printf(TEXT("Subgraph not found: '%s' (in path '%s')"), *Segment, *SubgraphPath)
			);
			return nullptr;
		}
	}

	return CurrentGraph;
}

void FCortexGraphNodeOps::CollectSubgraphsRecursive(
	UEdGraph* Graph,
	const FString& ParentGraphName,
	const FString& CurrentSubgraphPath,
	TArray<TSharedPtr<FJsonValue>>& OutArray,
	int32 Depth)
{
	if (!Graph || Depth > MaxSubgraphDepth)
	{
		return;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!IsValid(Node))
		{
			continue;
		}
		UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node);
		if (!CompositeNode || !CompositeNode->BoundGraph)
		{
			continue;
		}

		UEdGraph* Sub = CompositeNode->BoundGraph;
		if (!IsValid(Sub))
		{
			continue;
		}
		FString SubPath = CurrentSubgraphPath.IsEmpty()
			? Sub->GetName()
			: FString::Printf(TEXT("%s.%s"), *CurrentSubgraphPath, *Sub->GetName());

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Sub->GetName());
		Entry->SetStringField(TEXT("class"), Sub->GetClass()->GetName());
		Entry->SetNumberField(TEXT("node_count"), Sub->Nodes.Num());
		Entry->SetStringField(TEXT("parent_graph"), ParentGraphName);
		Entry->SetStringField(TEXT("subgraph_path"), SubPath);
		OutArray.Add(MakeShared<FJsonValueObject>(Entry));

		// Recurse
		CollectSubgraphsRecursive(Sub, Sub->GetName(), SubPath, OutArray, Depth + 1);
	}
}

FCortexCommandResult FCortexGraphNodeOps::ListGraphs(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	TArray<TSharedPtr<FJsonValue>> GraphsArray;

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph == nullptr)
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Graph->GetName());
		Entry->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
		Entry->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		GraphsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph == nullptr)
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Graph->GetName());
		Entry->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
		Entry->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		GraphsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Optionally include composite subgraphs
	bool bIncludeSubgraphs = false;
	Params->TryGetBoolField(TEXT("include_subgraphs"), bIncludeSubgraphs);
	if (bIncludeSubgraphs)
	{
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph)
			{
				CollectSubgraphsRecursive(Graph, Graph->GetName(), TEXT(""), GraphsArray, 0);
			}
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph)
			{
				CollectSubgraphsRecursive(Graph, Graph->GetName(), TEXT(""), GraphsArray, 0);
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("graphs"), GraphsArray);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphNodeOps::ListNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	UEdGraph* Graph = FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
	}

	// Resolve subgraph path if provided
	FString SubgraphPath;
	Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
	if (!SubgraphPath.IsEmpty())
	{
		Graph = ResolveSubgraph(Graph, SubgraphPath, LoadError);
		if (Graph == nullptr)
		{
			return LoadError;
		}
	}

	// compact=true by default: omit position, node_class, pin_count
	bool bCompact = true;
	Params->TryGetBoolField(TEXT("compact"), bCompact);

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node == nullptr)
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("node_id"), Node->GetName());
		const FString ClassName = Node->GetClass()->GetName();
		Entry->SetStringField(TEXT("class"), ClassName);
		if (!bCompact)
		{
			Entry->SetStringField(TEXT("node_class"), ClassName);
		}
		Entry->SetStringField(TEXT("display_name"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		if (!bCompact)
		{
			TSharedRef<FJsonObject> Pos = MakeShared<FJsonObject>();
			Pos->SetNumberField(TEXT("x"), Node->NodePosX);
			Pos->SetNumberField(TEXT("y"), Node->NodePosY);
			Entry->SetObjectField(TEXT("position"), Pos);
			Entry->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
		}

		int32 ConnectedPinCount = 0;
		int32 ConnectionCount = 0;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && Pin->LinkedTo.Num() > 0)
			{
				++ConnectedPinCount;
				ConnectionCount += Pin->LinkedTo.Num();
			}
		}
		Entry->SetNumberField(TEXT("connected_pin_count"), ConnectedPinCount);
		Entry->SetNumberField(TEXT("connections"), ConnectionCount);

		// Annotate composite nodes with their subgraph name
		UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node);
		if (CompositeNode && CompositeNode->BoundGraph)
		{
			Entry->SetStringField(TEXT("subgraph_name"), CompositeNode->BoundGraph->GetName());
		}

		// Annotate tunnel boundary nodes (entry/exit inside composites)
		// UK2Node_Composite IS-A UK2Node_Tunnel; use exact class check to identify
		// pure tunnel entry/exit nodes only (matching Epic's own convention)
		if (Node->GetClass() == UK2Node_Tunnel::StaticClass())
		{
			Entry->SetBoolField(TEXT("is_tunnel_boundary"), true);
		}

		NodesArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("nodes"), NodesArray);
	Data->SetNumberField(TEXT("node_count"), NodesArray.Num());
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphNodeOps::GetNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: node_id")
		);
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	UEdGraph* Graph = FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
	}

	// Resolve subgraph path if provided
	FString SubgraphPath;
	Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
	if (!SubgraphPath.IsEmpty())
	{
		Graph = ResolveSubgraph(Graph, SubgraphPath, LoadError);
		if (Graph == nullptr)
		{
			return LoadError;
		}
	}

	UEdGraphNode* Node = FindNode(Graph, NodeId, LoadError);
	if (Node == nullptr)
	{
		return LoadError;
	}

	// compact=true by default: omit position, node_class; filter hidden unconnected pins
	bool bCompact = true;
	Params->TryGetBoolField(TEXT("compact"), bCompact);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), Node->GetName());
	const FString ClassName = Node->GetClass()->GetName();
	Data->SetStringField(TEXT("class"), ClassName);
	if (!bCompact)
	{
		Data->SetStringField(TEXT("node_class"), ClassName);
	}
	Data->SetStringField(TEXT("display_name"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

	if (!bCompact)
	{
		TSharedRef<FJsonObject> Pos = MakeShared<FJsonObject>();
		Pos->SetNumberField(TEXT("x"), Node->NodePosX);
		Pos->SetNumberField(TEXT("y"), Node->NodePosY);
		Data->SetObjectField(TEXT("position"), Pos);
	}

	TArray<TSharedPtr<FJsonValue>> PinsArray;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin == nullptr)
		{
			continue;
		}
		if (bCompact && ShouldSkipPinCompact(Pin))
		{
			continue;
		}
		PinsArray.Add(MakeShared<FJsonValueObject>(SerializePin(Pin, true, bCompact)));
	}
	Data->SetArrayField(TEXT("pins"), PinsArray);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphNodeOps::SearchNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	FString NodeClass;
	FString FunctionName;
	FString DisplayName;
	Params->TryGetStringField(TEXT("node_class"), NodeClass);
	Params->TryGetStringField(TEXT("function_name"), FunctionName);
	Params->TryGetStringField(TEXT("display_name"), DisplayName);

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	FString SubgraphPath;
	Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);

	if (NodeClass.IsEmpty() && FunctionName.IsEmpty() && DisplayName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("At least one filter required: node_class, function_name, or display_name")
		);
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	// compact=true by default: omit node_class from results
	bool bCompact = true;
	Params->TryGetBoolField(TEXT("compact"), bCompact);

	TArray<TSharedPtr<FJsonValue>> ResultsArray;

	// Lambda: search a single graph, optionally recursing into composites
	TFunction<void(UEdGraph*, const FString&, int32)> SearchGraphRecursive;
	SearchGraphRecursive = [&](UEdGraph* Graph, const FString& CurrentSubgraphPath, int32 Depth)
	{
		if (Graph == nullptr || Depth > MaxSubgraphDepth)
		{
			return;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}

			// Recurse into composites
			UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node);
			if (CompositeNode && CompositeNode->BoundGraph)
			{
				FString ChildPath = CurrentSubgraphPath.IsEmpty()
					? CompositeNode->BoundGraph->GetName()
					: FString::Printf(TEXT("%s.%s"), *CurrentSubgraphPath, *CompositeNode->BoundGraph->GetName());
				SearchGraphRecursive(CompositeNode->BoundGraph, ChildPath, Depth + 1);
			}

			// Apply filters
			if (!NodeClass.IsEmpty())
			{
				const FString RuntimeClassName = Node->GetClass()->GetName();
				const FString FilterNoPrefix = NodeClass.StartsWith(TEXT("U")) ? NodeClass.Mid(1) : NodeClass;
				if (RuntimeClassName != NodeClass && RuntimeClassName != FilterNoPrefix)
				{
					continue;
				}
			}

			if (!DisplayName.IsEmpty())
			{
				const FString NodeDisplayName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				if (!NodeDisplayName.Contains(DisplayName, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}

			if (!FunctionName.IsEmpty())
			{
				const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
				if (CallNode == nullptr)
				{
					continue;
				}

				FString FunctionNameToMatch;
				if (const UFunction* TargetFunction = CallNode->GetTargetFunction())
				{
					FunctionNameToMatch = TargetFunction->GetName();
				}
				else
				{
					FunctionNameToMatch = CallNode->FunctionReference.GetMemberName().ToString();
				}

				if (!FunctionNameToMatch.Contains(FunctionName, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("node_id"), Node->GetName());
			const FString SearchNodeClass = Node->GetClass()->GetName();
			Entry->SetStringField(TEXT("class"), SearchNodeClass);
			if (!bCompact)
			{
				Entry->SetStringField(TEXT("node_class"), SearchNodeClass);
			}
			Entry->SetStringField(TEXT("display_name"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			Entry->SetStringField(TEXT("graph_name"), Graph->GetName());
			if (!CurrentSubgraphPath.IsEmpty())
			{
				Entry->SetStringField(TEXT("subgraph_path"), CurrentSubgraphPath);
			}
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	};

	if (!SubgraphPath.IsEmpty())
	{
		// Search within a specific subgraph only
		UEdGraph* RootGraph = FindGraph(Blueprint, GraphName, LoadError);
		if (RootGraph == nullptr)
		{
			return LoadError;
		}
		UEdGraph* TargetGraph = ResolveSubgraph(RootGraph, SubgraphPath, LoadError);
		if (TargetGraph == nullptr)
		{
			return LoadError;
		}
		SearchGraphRecursive(TargetGraph, SubgraphPath, 0);
	}
	else
	{
		// Search all top-level graphs, recursively descending into composites
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			SearchGraphRecursive(Graph, TEXT(""), 0);
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			SearchGraphRecursive(Graph, TEXT(""), 0);
		}
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			SearchGraphRecursive(Graph, TEXT(""), 0);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetNumberField(TEXT("count"), ResultsArray.Num());
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphNodeOps::AddNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	FString NodeClassName;
	if (!Params->TryGetStringField(TEXT("node_class"), NodeClassName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: node_class")
		);
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 PosX = 0;
	int32 PosY = 0;
	const TSharedPtr<FJsonObject>* PosObj = nullptr;
	if (Params->TryGetObjectField(TEXT("position"), PosObj) && PosObj)
	{
		(*PosObj)->TryGetNumberField(TEXT("x"), PosX);
		(*PosObj)->TryGetNumberField(TEXT("y"), PosY);
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	UEdGraph* Graph = FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
	}

	// Resolve subgraph path if provided
	FString SubgraphPath;
	Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
	if (!SubgraphPath.IsEmpty())
	{
		Graph = ResolveSubgraph(Graph, SubgraphPath, LoadError);
		if (Graph == nullptr)
		{
			return LoadError;
		}
	}

	// Resolve node class
	// For well-known classes, use StaticClass (faster, no dynamic loading)
	// Other classes use dynamic loading from /Script/BlueprintGraph or /Script/Engine
	UClass* NodeClass = nullptr;
	if (NodeClassName == TEXT("UK2Node_CallFunction"))
	{
		NodeClass = UK2Node_CallFunction::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_IfThenElse"))
	{
		NodeClass = UK2Node_IfThenElse::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_VariableSet"))
	{
		NodeClass = UK2Node_VariableSet::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_VariableGet"))
	{
		NodeClass = UK2Node_VariableGet::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_Event"))
	{
		NodeClass = UK2Node_Event::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_ExecutionSequence"))
	{
		NodeClass = UK2Node_ExecutionSequence::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_CustomEvent"))
	{
		NodeClass = UK2Node_CustomEvent::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_Self"))
	{
		NodeClass = UK2Node_Self::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_Knot"))
	{
		NodeClass = UK2Node_Knot::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_MakeArray"))
	{
		NodeClass = UK2Node_MakeArray::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_Timeline"))
	{
		// Timeline requires a UTimelineTemplate in Blueprint->Timelines with matching name/guid.
		// Adding without one produces a compile error.
		const TSharedPtr<FJsonObject>* NodeParams = nullptr;
		FString TimelineName;
		if (!Params->TryGetObjectField(TEXT("params"), NodeParams) ||
			!(*NodeParams)->TryGetStringField(TEXT("timeline_name"), TimelineName) ||
			TimelineName.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("TimelineNameRequired: timeline_name param is required for Timeline nodes")
			);
		}
		NodeClass = UK2Node_Timeline::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_SpawnActorFromClass"))
	{
		NodeClass = UK2Node_SpawnActorFromClass::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_DynamicCast"))
	{
		NodeClass = UK2Node_DynamicCast::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_MacroInstance"))
	{
		// MacroInstance requires SetMacroGraph(); adding without macro_path produces a node with no pins.
		const TSharedPtr<FJsonObject>* NodeParams = nullptr;
		FString MacroPath;
		if (!Params->TryGetObjectField(TEXT("params"), NodeParams) ||
			!(*NodeParams)->TryGetStringField(TEXT("macro_path"), MacroPath) ||
			MacroPath.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("MacroPathRequired: macro_path param is required for MacroInstance nodes")
			);
		}
		NodeClass = UK2Node_MacroInstance::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_SwitchEnum"))
	{
		NodeClass = UK2Node_SwitchEnum::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_SwitchString"))
	{
		NodeClass = UK2Node_SwitchString::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_SwitchInteger"))
	{
		NodeClass = UK2Node_SwitchInteger::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_AddDelegate"))
	{
		const TSharedPtr<FJsonObject>* NodeParams = nullptr;
		FString DelegateName;
		if (!Params->TryGetObjectField(TEXT("params"), NodeParams) ||
			!(*NodeParams)->TryGetStringField(TEXT("delegate_name"), DelegateName) ||
			DelegateName.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("DelegateNameRequired: delegate_name param is required for AddDelegate nodes")
			);
		}
		NodeClass = UK2Node_AddDelegate::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_RemoveDelegate"))
	{
		const TSharedPtr<FJsonObject>* NodeParams = nullptr;
		FString DelegateName;
		if (!Params->TryGetObjectField(TEXT("params"), NodeParams) ||
			!(*NodeParams)->TryGetStringField(TEXT("delegate_name"), DelegateName) ||
			DelegateName.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("DelegateNameRequired: delegate_name param is required for RemoveDelegate nodes")
			);
		}
		NodeClass = UK2Node_RemoveDelegate::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_ClearDelegate"))
	{
		const TSharedPtr<FJsonObject>* NodeParams = nullptr;
		FString DelegateName;
		if (!Params->TryGetObjectField(TEXT("params"), NodeParams) ||
			!(*NodeParams)->TryGetStringField(TEXT("delegate_name"), DelegateName) ||
			DelegateName.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("DelegateNameRequired: delegate_name param is required for ClearDelegate nodes")
			);
		}
		NodeClass = UK2Node_ClearDelegate::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_CreateDelegate"))
	{
		NodeClass = UK2Node_CreateDelegate::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_Composite") || NodeClassName == TEXT("Composite"))
	{
		NodeClass = UK2Node_Composite::StaticClass();
	}

	if (NodeClass == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Node class not found: %s"), *NodeClassName)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Add node %s"), *NodeClassName)
	));

	Graph->Modify();

	UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);
	NewNode->CreateNewGuid();
	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;
	Graph->AddNode(NewNode, true, false);

	// Handle type-specific setup
	// NodeParams = node-specific parameters (nested object), distinct from outer Params
	const TSharedPtr<FJsonObject>* NodeParams = nullptr;
	if (Params->TryGetObjectField(TEXT("params"), NodeParams) && NodeParams)
	{
		UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(NewNode);
		if (CallNode)
		{
			FString FunctionName;
			if ((*NodeParams)->TryGetStringField(TEXT("function_name"), FunctionName))
			{
				// Parse "ClassName.FunctionName" format
				FString ClassName;
				FString FuncName;
				if (FunctionName.Split(TEXT("."), &ClassName, &FuncName))
				{
					UClass* FuncClass = FindFirstObject<UClass>(*ClassName);
					if (FuncClass == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Function owner class not found: %s"), *ClassName)
						);
					}

					UFunction* Func = FuncClass->FindFunctionByName(FName(*FuncName));
					if (Func == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Function not found: %s on class %s"), *FuncName, *ClassName)
						);
					}

					CallNode->SetFromFunction(Func);
				}
			}
		}

		UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(NewNode);
		if (VarNode)
		{
			FString VariableName;
			if ((*NodeParams)->TryGetStringField(TEXT("variable_name"), VariableName))
			{
				FString VariableClass;
				if ((*NodeParams)->TryGetStringField(TEXT("variable_class"), VariableClass))
				{
					// External class property (e.g., PlayerController.bShowMouseCursor)
					UClass* VarClass = FindFirstObject<UClass>(*VariableClass);
					if (VarClass == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Variable owner class not found: %s"), *VariableClass)
						);
					}

					FProperty* Prop = VarClass->FindPropertyByName(FName(*VariableName));
					if (Prop == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Property not found: %s on class %s"), *VariableName, *VariableClass)
						);
					}

					VarNode->SetFromProperty(Prop, false, VarClass);
				}
				else
				{
					// Self-context property (on the Blueprint's own class)
					UClass* SelfClass = Blueprint->SkeletonGeneratedClass
						? Blueprint->SkeletonGeneratedClass
						: Blueprint->GeneratedClass;
					if (SelfClass)
					{
						FProperty* SelfProp = SelfClass->FindPropertyByName(FName(*VariableName));
						if (SelfProp == nullptr)
						{
							Graph->RemoveNode(NewNode);
							return FCortexCommandRouter::Error(
								CortexErrorCodes::InvalidField,
								FString::Printf(TEXT("Self property not found: %s"), *VariableName)
							);
						}
					}
					VarNode->VariableReference.SetSelfMember(FName(*VariableName));
				}
			}
		}

		UK2Node_Event* EventNode = Cast<UK2Node_Event>(NewNode);
		if (EventNode)
		{
			FString FunctionName;
			if ((*NodeParams)->TryGetStringField(TEXT("function_name"), FunctionName))
			{
				FString ClassName;
				FString FuncName;
				if (FunctionName.Split(TEXT("."), &ClassName, &FuncName))
				{
					UClass* FuncClass = FindFirstObject<UClass>(*ClassName);
					if (FuncClass == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Event owner class not found: %s"), *ClassName)
						);
					}

					UFunction* Func = FuncClass->FindFunctionByName(FName(*FuncName));
					if (Func == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Event function not found: %s on class %s"), *FuncName, *ClassName)
						);
					}

					EventNode->EventReference.SetExternalMember(FName(*FuncName), FuncClass);
					EventNode->bOverrideFunction = true;
				}
			}
		}

		UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(NewNode);
		if (DelegateNode)
		{
			FString DelegateName;
			if ((*NodeParams)->TryGetStringField(TEXT("delegate_name"), DelegateName))
			{
				FString DelegateClass;
				if ((*NodeParams)->TryGetStringField(TEXT("delegate_class"), DelegateClass))
				{
					// External class delegate
					UClass* OwnerClass = FindFirstObject<UClass>(*DelegateClass);
					if (OwnerClass == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Delegate owner class not found: %s"), *DelegateClass)
						);
					}

					FMulticastDelegateProperty* DelegateProp = CastField<FMulticastDelegateProperty>(
						OwnerClass->FindPropertyByName(FName(*DelegateName)));
					if (DelegateProp == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Multicast delegate property not found: %s on class %s"),
								*DelegateName, *DelegateClass)
						);
					}

					DelegateNode->SetFromProperty(DelegateProp, false, OwnerClass);
				}
				else
				{
					// Self-context delegate (Blueprint's own event dispatcher)
					UClass* SelfClass = Blueprint->SkeletonGeneratedClass
						? Blueprint->SkeletonGeneratedClass
						: Blueprint->GeneratedClass;
					if (SelfClass == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							TEXT("Blueprint has no generated class for self-context delegate lookup")
						);
					}

					FMulticastDelegateProperty* DelegateProp = CastField<FMulticastDelegateProperty>(
						SelfClass->FindPropertyByName(FName(*DelegateName)));
					if (DelegateProp == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Self delegate property not found: %s"), *DelegateName)
						);
					}

					DelegateNode->SetFromProperty(DelegateProp, true, SelfClass);
				}
			}
		}

		UK2Node_CreateDelegate* CreateDelegateNode = Cast<UK2Node_CreateDelegate>(NewNode);
		if (CreateDelegateNode)
		{
			FString FunctionName;
			if ((*NodeParams)->TryGetStringField(TEXT("function_name"), FunctionName))
			{
				CreateDelegateNode->SetFunction(FName(*FunctionName));
			}
		}
	}

	NewNode->AllocateDefaultPins();

	// Special setup for composite nodes: PostPlacedNewNode creates the BoundGraph
	// and its tunnel entry/exit nodes. Without this call BoundGraph remains null and
	// ResolveSubgraph cannot traverse into the composite.
	UK2Node_Composite* CompositeNewNode = Cast<UK2Node_Composite>(NewNode);
	if (CompositeNewNode)
	{
		CompositeNewNode->PostPlacedNewNode();
		// Re-allocate pins after PostPlacedNewNode so the entry/exit tunnel pins are present
		CompositeNewNode->AllocateDefaultPins();
	}

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Force GUID resolution for CreateDelegate nodes — SetFunction alone leaves
	// SelectedFunctionGuid invalid, which is only resolved lazily via
	// PinConnectionListChanged/NodeConnectionListChanged in the editor.
	// Only call HandleAnyChange when no function name was pre-set, because
	// HandleAnyChange clears SelectedFunctionName when the function cannot be
	// resolved (common for programmatic creation before wiring).
	UK2Node_CreateDelegate* CreateDelegatePost = Cast<UK2Node_CreateDelegate>(NewNode);
	if (CreateDelegatePost && CreateDelegatePost->GetFunctionName() == NAME_None)
	{
		CreateDelegatePost->HandleAnyChange(true);
	}

	// Build response
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NewNode->GetName());
	const FString AddedNodeClass = NewNode->GetClass()->GetName();
	Data->SetStringField(TEXT("class"), AddedNodeClass);
	Data->SetStringField(TEXT("node_class"), AddedNodeClass);
	Data->SetStringField(TEXT("display_name"), NewNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

	TArray<TSharedPtr<FJsonValue>> PinsArray;
	for (UEdGraphPin* Pin : NewNode->Pins)
	{
		if (Pin == nullptr)
		{
			continue;
		}
		PinsArray.Add(MakeShared<FJsonValueObject>(SerializePin(Pin, false)));
	}
	Data->SetArrayField(TEXT("pins"), PinsArray);

	return FCortexCommandRouter::Success(Data);
}

bool FCortexGraphNodeOps::ShouldSkipPinCompact(const UEdGraphPin* Pin)
{
	if (Pin == nullptr)
	{
		return true;
	}
	return Pin->bHidden
		&& Pin->LinkedTo.Num() == 0
		&& Pin->DefaultValue.IsEmpty()
		&& Pin->DefaultTextValue.IsEmpty()
		&& Pin->DefaultObject == nullptr;
}

TSharedRef<FJsonObject> FCortexGraphNodeOps::SerializePin(const UEdGraphPin* Pin, bool bDetailed, bool bCompact)
{
	TSharedRef<FJsonObject> PinEntry = MakeShared<FJsonObject>();
	PinEntry->SetStringField(TEXT("name"), Pin->PinName.ToString());
	PinEntry->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
	PinEntry->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
	if (bDetailed)
	{
		const bool bIsConnected = Pin->LinkedTo.Num() > 0;

		// In compact mode, omit false is_connected and empty default_value
		if (!bCompact || !Pin->DefaultValue.IsEmpty())
		{
			PinEntry->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		}
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text && !Pin->DefaultTextValue.IsEmpty())
		{
			PinEntry->SetObjectField(TEXT("default_text_value"), FCortexSerializer::TextToJson(Pin->DefaultTextValue));
		}
		if (!bCompact || bIsConnected)
		{
			PinEntry->SetBoolField(TEXT("is_connected"), bIsConnected);
		}

		if (bIsConnected)
		{
			TArray<TSharedPtr<FJsonValue>> ConnArray;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin == nullptr || !IsValid(LinkedPin->GetOwningNode()))
				{
					continue;
				}

				TSharedRef<FJsonObject> Conn = MakeShared<FJsonObject>();
				Conn->SetStringField(TEXT("node_id"), LinkedPin->GetOwningNode()->GetName());
				Conn->SetStringField(TEXT("pin"), LinkedPin->PinName.ToString());
				ConnArray.Add(MakeShared<FJsonValueObject>(Conn));
			}

			PinEntry->SetArrayField(TEXT("connections"), ConnArray);
		}
	}
	return PinEntry;
}

FCortexCommandResult FCortexGraphNodeOps::RemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NodeId;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		&& Params->TryGetStringField(TEXT("node_id"), NodeId);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and node_id")
		);
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	UEdGraph* Graph = FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
	}

	// Resolve subgraph path if provided
	FString SubgraphPath;
	Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
	if (!SubgraphPath.IsEmpty())
	{
		Graph = ResolveSubgraph(Graph, SubgraphPath, LoadError);
		if (Graph == nullptr)
		{
			return LoadError;
		}
	}

	// Find the node
	UEdGraphNode* FoundNode = FindNode(Graph, NodeId, LoadError);
	if (FoundNode == nullptr)
	{
		return LoadError;
	}

	// Count connected pins before removal
	int32 DisconnectedPins = 0;
	for (UEdGraphPin* Pin : FoundNode->Pins)
	{
		if (Pin != nullptr && Pin->LinkedTo.Num() > 0)
		{
			++DisconnectedPins;
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Remove node %s"), *NodeId)
	));
	Graph->Modify();

	// Break all connections then remove the node
	FoundNode->BreakAllNodeLinks();
	Graph->RemoveNode(FoundNode);

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("removed_node_id"), NodeId);
	Data->SetNumberField(TEXT("disconnected_pins"), DisconnectedPins);

	UE_LOG(LogCortexGraph, Log, TEXT("Removed node %s from graph %s (%d pins disconnected)"),
		*NodeId, *Graph->GetName(), DisconnectedPins);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphNodeOps::SetPinValue(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NodeId;
	FString PinName;
	FString Value;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		&& Params->TryGetStringField(TEXT("node_id"), NodeId)
		&& Params->TryGetStringField(TEXT("pin_name"), PinName)
		&& Params->TryGetStringField(TEXT("value"), Value);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, node_id, pin_name, and value")
		);
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	UEdGraph* Graph = FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
	}

	// Resolve subgraph path if provided
	FString SubgraphPath;
	Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
	if (!SubgraphPath.IsEmpty())
	{
		Graph = ResolveSubgraph(Graph, SubgraphPath, LoadError);
		if (Graph == nullptr)
		{
			return LoadError;
		}
	}

	UEdGraphNode* Node = FindNode(Graph, NodeId, LoadError);
	if (Node == nullptr)
	{
		return LoadError;
	}

	UEdGraphPin* Pin = FindPin(Node, PinName, LoadError);
	if (Pin == nullptr)
	{
		return LoadError;
	}

	// Verify this is an input pin (output pins don't have default values)
	if (Pin->Direction != EGPD_Input)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Cannot set value on output pin: %s"), *PinName)
		);
	}

	// Verify pin is not connected (connected pins ignore default values)
	if (Pin->LinkedTo.Num() > 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Cannot set value on connected pin: %s"), *PinName)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Set pin value %s.%s"), *NodeId, *PinName)
	));

	Graph->Modify();
	Node->Modify();

	// Set the default value
	// For class/object pins, try to resolve and set DefaultObject
	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
	{
		// Try to load the class from the value string
		UClass* ClassObject = LoadClass<UObject>(nullptr, *Value);
		if (ClassObject)
		{
			Pin->DefaultObject = ClassObject;
			Pin->DefaultValue = TEXT("");  // Clear string value when using object reference
		}
		else
		{
			// Fallback to string value if class can't be loaded
			Pin->DefaultValue = Value;
		}
	}
	else
	{
		Pin->DefaultValue = Value;
	}

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetStringField(TEXT("pin_name"), PinName);
	Data->SetStringField(TEXT("value"), Value);
	Data->SetBoolField(TEXT("success"), true);

	UE_LOG(LogCortexGraph, Log, TEXT("Set pin value %s.%s = %s"), *NodeId, *PinName, *Value);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphNodeOps::AutoLayout(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint) return LoadError;

	FString ModeStr;
	Params->TryGetStringField(TEXT("mode"), ModeStr);
	ECortexLayoutMode Mode = (ModeStr == TEXT("incremental"))
		? ECortexLayoutMode::Incremental : ECortexLayoutMode::Full;

	FString GraphFilter;
	Params->TryGetStringField(TEXT("graph_name"), GraphFilter);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;
	Config.Mode = Mode;

	double HSpacingVal = 0, VSpacingVal = 0;
	if (Params->TryGetNumberField(TEXT("horizontal_spacing"), HSpacingVal) && HSpacingVal > 0)
	{
		Config.HorizontalSpacing = static_cast<int32>(HSpacingVal);
	}
	if (Params->TryGetNumberField(TEXT("vertical_spacing"), VSpacingVal) && VSpacingVal > 0)
	{
		Config.VerticalSpacing = static_cast<int32>(VSpacingVal);
	}

	FString SubgraphPath;
	Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);

	// Collect graphs to process
	TArray<UEdGraph*> Graphs;
	if (!GraphFilter.IsEmpty())
	{
		UEdGraph* Graph = FindGraph(Blueprint, GraphFilter, LoadError);
		if (!Graph) return LoadError;

		// Resolve subgraph path if provided
		if (!SubgraphPath.IsEmpty())
		{
			Graph = ResolveSubgraph(Graph, SubgraphPath, LoadError);
			if (Graph == nullptr)
			{
				return LoadError;
			}
		}

		Graphs.Add(Graph);
	}
	else
	{
		for (UEdGraph* G : Blueprint->UbergraphPages) { if (G) Graphs.Add(G); }
		for (UEdGraph* G : Blueprint->FunctionGraphs) { if (G) Graphs.Add(G); }
		for (UEdGraph* G : Blueprint->MacroGraphs) { if (G) Graphs.Add(G); }
	}

	int32 TotalNodesProcessed = 0;

	TUniquePtr<FScopedTransaction> Transaction;
	if (!FCortexCommandRouter::IsInBatch())
	{
		Transaction = MakeUnique<FScopedTransaction>(
			FText::FromString(TEXT("Cortex: Auto-Layout Blueprint Graphs")));
	}

	for (UEdGraph* Graph : Graphs)
	{
		TArray<FCortexLayoutNode> LayoutNodes;
		TMap<FString, UEdGraphNode*> IdToNode;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			// Skip comment nodes (class name contains "Comment")
			if (Node->GetClass()->GetName() == TEXT("EdGraphNode_Comment"))
			{
				continue;
			}

			FCortexLayoutNode LN;
			LN.Id = Node->GetName();
			IdToNode.Add(LN.Id, Node);

			bool bHasExecInput = false;
			bool bHasExecOutput = false;
			int32 InputPinCount = 0;
			int32 OutputPinCount = 0;

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				bool bIsExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
				if (Pin->Direction == EGPD_Input)
				{
					InputPinCount++;
					if (bIsExec) bHasExecInput = true;
				}
				else
				{
					OutputPinCount++;
					if (bIsExec) bHasExecOutput = true;
				}
			}

			LN.bIsEntryPoint = (!bHasExecInput && bHasExecOutput);
			LN.bIsExecNode = (bHasExecInput || bHasExecOutput);
			int32 PinRows = FMath::Max(InputPinCount, OutputPinCount);
			LN.Width = 200;
			LN.Height = FMath::Max(100, PinRows * 28 + 40);

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				bool bIsExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
					FString TargetId = LinkedPin->GetOwningNode()->GetName();
					if (bIsExec)
					{
						LN.ExecOutputs.AddUnique(TargetId);
					}
					else
					{
						LN.DataOutputs.AddUnique(TargetId);
					}
				}
			}
			LayoutNodes.Add(LN);
		}

		// Collect existing positions for incremental mode
		TMap<FString, FIntPoint> ExistingPositions;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && !Node->GetClass()->GetName().Contains(TEXT("Comment")))
			{
				ExistingPositions.Add(Node->GetName(), FIntPoint(Node->NodePosX, Node->NodePosY));
			}
		}

		FCortexLayoutResult LayoutResult = FCortexGraphLayoutOps::CalculateLayout(
			LayoutNodes, Config, ExistingPositions);

		for (const auto& Pair : LayoutResult.Positions)
		{
			UEdGraphNode** NodePtr = IdToNode.Find(Pair.Key);
			if (NodePtr && *NodePtr)
			{
				(*NodePtr)->Modify();
				(*NodePtr)->NodePosX = Pair.Value.X;
				(*NodePtr)->NodePosY = Pair.Value.Y;
			}
		}

		if (FCortexCommandRouter::IsInBatch())
		{
			FString GraphKey = FString::Printf(TEXT("graph.notify.%s"), *Graph->GetPathName());
			FCortexBatchScope::AddCleanupAction(GraphKey,
				[WeakGraph = TWeakObjectPtr<UEdGraph>(Graph)]()
				{
					if (UEdGraph* G = WeakGraph.Get()) G->NotifyGraphChanged();
				});
		}
		else
		{
			Graph->NotifyGraphChanged();
		}

		TotalNodesProcessed += LayoutResult.Positions.Num();
	}

	if (FCortexCommandRouter::IsInBatch())
	{
		FString BPKey = FString::Printf(TEXT("blueprint.modified.%s"), *Blueprint->GetPathName());
		FCortexBatchScope::AddCleanupAction(BPKey,
			[WeakBP = TWeakObjectPtr<UBlueprint>(Blueprint)]()
			{
				if (UBlueprint* BP = WeakBP.Get())
				{
					FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
				}
			});
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
	Blueprint->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("node_count"), TotalNodesProcessed);
	Data->SetNumberField(TEXT("graphs_processed"), Graphs.Num());

	UE_LOG(LogCortexGraph, Log, TEXT("Auto-layout completed: %d nodes across %d graphs in %s"),
		TotalNodesProcessed, Graphs.Num(), *AssetPath);

	return FCortexCommandRouter::Success(Data);
}
