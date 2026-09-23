#include "Operations/CortexGraphNodeOps.h"
#include "Operations/CortexGraphNodeContract.h"
#include "Operations/CortexGraphPinDefaults.h"
#include "Operations/CortexGraphSymbolResolver.h"
#include "CortexAssetFingerprint.h"
#include "CortexBatchMutation.h"
#include "CortexGraphModule.h"
#include "CortexSerializer.h"
#include "CortexEditorUtils.h"
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
#include "WidgetBlueprint.h"
#include "ScopedTransaction.h"
#include "PackageTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/LevelScriptBlueprint.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace
{
FString NormalizeGraphBlueprintAssetPath(const FString& AssetPath)
{
	return FCortexEditorUtils::NormalizeMountedContentPath(AssetPath);
}

FString GetGraphNodeWritableValidationPath(const FString& AssetPath)
{
	static const FString LevelBPPrefix = TEXT("__level_bp__:");
	const FString BlueprintPath = AssetPath.StartsWith(LevelBPPrefix)
		? AssetPath.Mid(LevelBPPrefix.Len())
		: AssetPath;
	return FPackageName::ObjectPathToPackageName(NormalizeGraphBlueprintAssetPath(BlueprintPath));
}

bool DoesGraphBlueprintPackageExist(const FString& PackagePath)
{
	if (FindPackage(nullptr, *PackagePath) != nullptr)
	{
		return true;
	}

	FString PackageFilename;
	return FPackageName::TryConvertLongPackageNameToFilename(
			PackagePath,
			PackageFilename,
			FPackageName::GetAssetPackageExtension())
		&& FPackageName::DoesPackageExist(PackagePath);
}

bool ValidateWritableGraphNodeBlueprintAssetPath(const FString& AssetPath, FCortexCommandResult& OutError)
{
	FString ValidationError;
	if (!FCortexEditorUtils::IsWritableMountedContentPath(GetGraphNodeWritableValidationPath(AssetPath), ValidationError))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
		return false;
	}

	return true;
}

bool ResolveMutableNodeGraph(
	UBlueprint* Blueprint,
	const FString& GraphName,
	UEdGraph*& OutGraph,
	FCortexCommandResult& OutError)
{
	FCortexGraphEntry Entry;
	if (!FCortexGraphNodeOps::FindGraphEntry(Blueprint, GraphName, Entry))
	{
		const FString TargetName = GraphName.IsEmpty() ? TEXT("EventGraph") : GraphName;
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::GraphNotFound,
			FString::Printf(TEXT("Graph not found: %s"), *TargetName)
		);
		return false;
	}

	if (!FCortexGraphNodeOps::IsMutableGraphKind(Entry.Kind))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(
				TEXT("Graph kind is not mutable through graph_cmd: %s"),
				*FCortexGraphNodeOps::GraphKindToString(Entry.Kind))
		);
		return false;
	}

	OutGraph = Entry.Graph;
	return true;
}

bool TryParseGraphKind(const FString& GraphKindString, ECortexGraphKind& OutKind)
{
	if (GraphKindString == TEXT("ubergraph"))
	{
		OutKind = ECortexGraphKind::Ubergraph;
		return true;
	}
	if (GraphKindString == TEXT("function"))
	{
		OutKind = ECortexGraphKind::Function;
		return true;
	}
	if (GraphKindString == TEXT("macro"))
	{
		OutKind = ECortexGraphKind::Macro;
		return true;
	}
	if (GraphKindString == TEXT("delegate"))
	{
		OutKind = ECortexGraphKind::Delegate;
		return true;
	}
	if (GraphKindString == TEXT("interface_impl"))
	{
		OutKind = ECortexGraphKind::InterfaceImpl;
		return true;
	}

	return false;
}

bool ResolveMutableNodeGraphForSetPinValue(
	UBlueprint* Blueprint,
	const FString& GraphName,
	const FString& GraphKindString,
	const FString& OwningInterfaceString,
	const bool bStructuredTextWrite,
	UEdGraph*& OutGraph,
	FCortexCommandResult& OutError)
{
	const FString TargetName = GraphName.IsEmpty() ? TEXT("EventGraph") : GraphName;
	TArray<FCortexGraphEntry> Entries;
	FCortexGraphNodeOps::EnumerateUserGraphs(Blueprint, Entries);

	TArray<FCortexGraphEntry> MatchingEntries;
	for (const FCortexGraphEntry& Entry : Entries)
	{
		if (Entry.Graph == nullptr || Entry.Graph->GetName() != TargetName)
		{
			continue;
		}
		if (!FCortexGraphNodeOps::IsMutableGraphKind(Entry.Kind))
		{
			continue;
		}
		MatchingEntries.Add(Entry);
	}

	if (!GraphKindString.IsEmpty())
	{
		ECortexGraphKind RequestedKind = ECortexGraphKind::Function;
		if (!TryParseGraphKind(GraphKindString, RequestedKind))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Unknown graph_kind: %s"), *GraphKindString));
			return false;
		}

		MatchingEntries = MatchingEntries.FilterByPredicate(
			[RequestedKind](const FCortexGraphEntry& Entry)
			{
				return Entry.Kind == RequestedKind;
			});
	}

	if (!OwningInterfaceString.IsEmpty())
	{
		MatchingEntries = MatchingEntries.FilterByPredicate(
			[&OwningInterfaceString](const FCortexGraphEntry& Entry)
			{
				return Entry.OwningInterface.ToString() == OwningInterfaceString;
			});
	}

	if (MatchingEntries.Num() == 0)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::GraphNotFound,
			FString::Printf(TEXT("Graph not found: %s"), *TargetName));
		return false;
	}

	if (bStructuredTextWrite && MatchingEntries.Num() > 1)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Graph name '%s' is ambiguous for persisted write; pass graph_kind or owning_interface"), *TargetName));
		return false;
	}

	OutGraph = MatchingEntries[0].Graph;
	return true;
}

bool ReloadBlueprintPackage(UPackage* Package)
{
	if (Package == nullptr)
	{
		return false;
	}

	TArray<UPackage*> PackagesToReload;
	PackagesToReload.Add(Package);

	FText ReloadError;
	return UPackageTools::ReloadPackages(
		PackagesToReload,
		ReloadError,
		EReloadPackagesInteractionMode::AssumePositive);
}

TSharedPtr<FJsonObject> AllocateProbePins(
	UBlueprint* Blueprint,
	const FString& NodeClassName,
	const TSharedPtr<FJsonObject>& NodeParams,
	UClass* NodeClass)
{
	if (NodeClass == nullptr)
	{
		return nullptr;
	}
	UEdGraph* ProbeGraph = NewObject<UEdGraph>(Blueprint, UEdGraph::StaticClass(), NAME_None, RF_Transient);
	// K2 nodes dereference the graph schema during AllocateDefaultPins/ReconstructNode; a null
	// schema here would crash describe_node when construction params are provided.
	ProbeGraph->Schema = UEdGraphSchema_K2::StaticClass();
	UEdGraphNode* ProbeNode = NewObject<UEdGraphNode>(ProbeGraph, NodeClass, NAME_None, RF_Transient);
	FString ApplyError;
	FCortexGraphNodeContract::ApplyNodeConstructionParams(ProbeGraph, ProbeNode, Blueprint, NodeParams, ApplyError);
	if (ProbeNode->Pins.Num() == 0)
	{
		ProbeNode->AllocateDefaultPins();
	}

	if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(ProbeNode))
	{
		Composite->PostPlacedNewNode();
		Composite->AllocateDefaultPins();
	}

	TSharedPtr<FJsonObject> Pins = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PinArray;
	for (const UEdGraphPin* Pin : ProbeNode->Pins)
	{
		if (Pin == nullptr)
		{
			continue;
		}
		PinArray.Add(MakeShared<FJsonValueObject>(FCortexGraphNodeOps::SerializePin(Pin, false, false, Blueprint)));
	}
	Pins->SetArrayField(TEXT("pins"), PinArray);

	ProbeNode->MarkAsGarbage();
	ProbeGraph->MarkAsGarbage();

	return Pins;
}

}

UBlueprint* FCortexGraphNodeOps::LoadBlueprint(const FString& AssetPath, FCortexCommandResult& OutError)
{
	// Level Script Blueprint: synthetic path __level_bp__:/Game/Maps/MapName
	static const FString LevelBPPrefix = TEXT("__level_bp__:");
	if (AssetPath.StartsWith(LevelBPPrefix))
	{
		const FString MapPath = FPackageName::ObjectPathToPackageName(
			NormalizeGraphBlueprintAssetPath(AssetPath.Mid(LevelBPPrefix.Len())));

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

	const FString NormalizedPath = NormalizeGraphBlueprintAssetPath(AssetPath);

	// Check if package exists before LoadObject to avoid SkipPackage warnings
	const FString PkgName = FPackageName::ObjectPathToPackageName(NormalizedPath);
	if (!DoesGraphBlueprintPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Blueprint not found: %s"), *NormalizedPath)
		);
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *NormalizedPath);
	if (Blueprint == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Blueprint not found: %s"), *NormalizedPath)
		);
	}
	return Blueprint;
}

void FCortexGraphNodeOps::EnumerateUserGraphs(UBlueprint* Blueprint, TArray<FCortexGraphEntry>& OutEntries)
{
	OutEntries.Reset();
	if (Blueprint == nullptr)
	{
		return;
	}

	auto AppendGraphs = [&OutEntries](const TArray<TObjectPtr<UEdGraph>>& Graphs, ECortexGraphKind Kind, FName OwningInterface)
	{
		for (const TObjectPtr<UEdGraph>& GraphPtr : Graphs)
		{
			if (UEdGraph* Graph = GraphPtr.Get())
			{
				FCortexGraphEntry Entry;
				Entry.Graph = Graph;
				Entry.Kind = Kind;
				Entry.OwningInterface = OwningInterface;
				OutEntries.Add(Entry);
			}
		}
	};

	AppendGraphs(Blueprint->UbergraphPages, ECortexGraphKind::Ubergraph, NAME_None);
	AppendGraphs(Blueprint->FunctionGraphs, ECortexGraphKind::Function, NAME_None);
	AppendGraphs(Blueprint->MacroGraphs, ECortexGraphKind::Macro, NAME_None);
	AppendGraphs(Blueprint->DelegateSignatureGraphs, ECortexGraphKind::Delegate, NAME_None);

	for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		const FName OwningInterfaceName = InterfaceDesc.Interface ? InterfaceDesc.Interface->GetFName() : NAME_None;
		AppendGraphs(InterfaceDesc.Graphs, ECortexGraphKind::InterfaceImpl, OwningInterfaceName);
	}
}

FString FCortexGraphNodeOps::GraphKindToString(ECortexGraphKind Kind)
{
	switch (Kind)
	{
		case ECortexGraphKind::Ubergraph:
			return TEXT("ubergraph");
		case ECortexGraphKind::Function:
			return TEXT("function");
		case ECortexGraphKind::Macro:
			return TEXT("macro");
		case ECortexGraphKind::Delegate:
			return TEXT("delegate");
		case ECortexGraphKind::InterfaceImpl:
			return TEXT("interface_impl");
	}

	return TEXT("function");
}

bool FCortexGraphNodeOps::FindGraphEntry(UBlueprint* Blueprint, const FString& GraphName, FCortexGraphEntry& OutEntry)
{
	const FString TargetName = GraphName.IsEmpty() ? TEXT("EventGraph") : GraphName;

	TArray<FCortexGraphEntry> Entries;
	EnumerateUserGraphs(Blueprint, Entries);
	for (const FCortexGraphEntry& Entry : Entries)
	{
		if (Entry.Graph && Entry.Graph->GetName() == TargetName)
		{
			OutEntry = Entry;
			return true;
		}
	}

	return false;
}

bool FCortexGraphNodeOps::IsMutableGraphKind(ECortexGraphKind Kind)
{
	switch (Kind)
	{
		case ECortexGraphKind::Ubergraph:
		case ECortexGraphKind::Function:
		case ECortexGraphKind::Macro:
		case ECortexGraphKind::InterfaceImpl:
			return true;
		case ECortexGraphKind::Delegate:
			return false;
	}

	return false;
}

UEdGraph* FCortexGraphNodeOps::FindGraph(UBlueprint* Blueprint, const FString& GraphName, FCortexCommandResult& OutError)
{
	FCortexGraphEntry Entry;
	if (FindGraphEntry(Blueprint, GraphName, Entry))
	{
		return Entry.Graph;
	}

	const FString TargetName = GraphName.IsEmpty() ? TEXT("EventGraph") : GraphName;
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

	TArray<FCortexGraphEntry> Entries;
	EnumerateUserGraphs(Blueprint, Entries);
	for (const FCortexGraphEntry& GraphEntry : Entries)
	{
		UEdGraph* Graph = GraphEntry.Graph;
		if (Graph == nullptr)
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Graph->GetName());
		Entry->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
		Entry->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		Entry->SetStringField(TEXT("kind"), GraphKindToString(GraphEntry.Kind));
		if (GraphEntry.Kind == ECortexGraphKind::InterfaceImpl && GraphEntry.OwningInterface != NAME_None)
		{
			Entry->SetStringField(TEXT("owning_interface"), GraphEntry.OwningInterface.ToString());
		}
		GraphsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Optionally include composite subgraphs
	bool bIncludeSubgraphs = false;
	Params->TryGetBoolField(TEXT("include_subgraphs"), bIncludeSubgraphs);
	if (bIncludeSubgraphs)
	{
		for (const FCortexGraphEntry& GraphEntry : Entries)
		{
			if (GraphEntry.Graph)
			{
				CollectSubgraphsRecursive(GraphEntry.Graph, GraphEntry.Graph->GetName(), TEXT(""), GraphsArray, 0);
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
		NodesArray.Add(MakeShared<FJsonValueObject>(SerializeNode(Node, false, bCompact)));
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

	return FCortexCommandRouter::Success(SerializeNode(Node, true, bCompact));
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
		TArray<FCortexGraphEntry> Entries;
		EnumerateUserGraphs(Blueprint, Entries);
		for (const FCortexGraphEntry& Entry : Entries)
		{
			SearchGraphRecursive(Entry.Graph, TEXT(""), 0);
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
	if (!ValidateWritableGraphNodeBlueprintAssetPath(AssetPath, LoadError))
	{
		return LoadError;
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	UEdGraph* Graph = nullptr;
	if (!ResolveMutableNodeGraph(Blueprint, GraphName, Graph, LoadError))
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

	UClass* NodeClass = ResolveNodeClass(NodeClassName);
	if (NodeClass == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Node class not found: %s"), *NodeClassName)
		);
	}

	// Node-specific construction params live under the top-level "params" key.
	const TSharedPtr<FJsonObject>* InnerNodeParams = nullptr;
	const bool bHasInnerParams = Params.IsValid()
		&& Params->TryGetObjectField(TEXT("params"), InnerNodeParams)
		&& InnerNodeParams != nullptr;
	const TSharedPtr<FJsonObject> NodeConstructionParams = bHasInnerParams
		? *InnerNodeParams : MakeShared<FJsonObject>();

	FCortexCommandResult ContractError;
	if (!FCortexGraphNodeContract::Validate(NodeClassName, Blueprint, NodeConstructionParams, ContractError))
	{
		return ContractError;
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

	FString ApplyError;
	if (!FCortexGraphNodeContract::ApplyNodeConstructionParams(Graph, NewNode, Blueprint, NodeConstructionParams, ApplyError))
	{
		Graph->RemoveNode(NewNode);
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ApplyError);
	}

	if (NewNode->Pins.Num() == 0)
	{
		NewNode->AllocateDefaultPins();
	}

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

	// Record the mutation for failure-atomic batch rollback: a failing batch removes every
	// node it created (reverse journal order) and verifies the graph is back to its prior state.
	if (FCortexCommandRouter::IsInBatch())
	{
		const TWeakObjectPtr<UBlueprint> WeakBlueprint = Blueprint;
		const FString RootGraphName = GraphName;
		const FString JournalSubgraphPath = SubgraphPath;
		const FGuid NodeGuid = NewNode->NodeGuid;
		const FString NodeId = NewNode->GetName();
		const UK2Node_Composite* JournalComposite = Cast<UK2Node_Composite>(NewNode);
		const FString BoundGraphName = JournalComposite && JournalComposite->BoundGraph
			? JournalComposite->BoundGraph->GetName() : FString();
		FCortexBatchScope::RegisterRollbackEntry(
			TEXT("add_node"),
			NodeId,
			FString::Printf(TEXT("add_node %s"), *NodeClassName),
			[WeakBlueprint, RootGraphName, JournalSubgraphPath, NodeGuid, NodeId]() -> bool
			{
				UBlueprint* CurrentBlueprint = WeakBlueprint.Get();
				if (CurrentBlueprint == nullptr)
				{
					return false;
				}
				FCortexCommandResult ResolveError;
				UEdGraph* CurrentGraph = FCortexGraphNodeOps::FindGraph(CurrentBlueprint, RootGraphName, ResolveError);
				if (CurrentGraph != nullptr && !JournalSubgraphPath.IsEmpty())
				{
					CurrentGraph = FCortexGraphNodeOps::ResolveSubgraph(CurrentGraph, JournalSubgraphPath, ResolveError);
				}
				if (CurrentGraph == nullptr)
				{
					return false;
				}
				UEdGraphNode* CurrentNode = nullptr;
				for (UEdGraphNode* Candidate : CurrentGraph->Nodes)
				{
					if (Candidate != nullptr && (Candidate->NodeGuid == NodeGuid || Candidate->GetName() == NodeId))
					{
						CurrentNode = Candidate;
						break;
					}
				}
				if (CurrentNode != nullptr)
				{
					CurrentNode->DestroyNode();
				}
				return true;
			},
			[WeakBlueprint, RootGraphName, JournalSubgraphPath, NodeGuid, NodeId, BoundGraphName]() -> bool
			{
				UBlueprint* CurrentBlueprint = WeakBlueprint.Get();
				if (CurrentBlueprint == nullptr)
				{
					return false;
				}
				FCortexCommandResult ResolveError;
				UEdGraph* CurrentGraph = FCortexGraphNodeOps::FindGraph(CurrentBlueprint, RootGraphName, ResolveError);
				if (CurrentGraph != nullptr && !JournalSubgraphPath.IsEmpty())
				{
					CurrentGraph = FCortexGraphNodeOps::ResolveSubgraph(CurrentGraph, JournalSubgraphPath, ResolveError);
				}
				if (CurrentGraph == nullptr)
				{
					return false;
				}
				for (UEdGraphNode* Node : CurrentGraph->Nodes)
				{
					if (Node != nullptr && (Node->NodeGuid == NodeGuid || Node->GetName() == NodeId))
					{
						return false;
					}
				}
				if (!BoundGraphName.IsEmpty())
				{
					for (const UEdGraph* SubGraph : CurrentGraph->SubGraphs)
					{
						if (SubGraph != nullptr && SubGraph->GetName() == BoundGraphName)
						{
							return false;
						}
					}
				}
				return true;
			},
			Cast<UPackage>(Graph->GetOutermost()));
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
		PinsArray.Add(MakeShared<FJsonValueObject>(SerializePin(Pin, false, false, Blueprint)));
	}
	Data->SetArrayField(TEXT("pins"), PinsArray);

	return FCortexCommandRouter::Success(Data);
}

UClass* FCortexGraphNodeOps::ResolveNodeClass(const FString& NodeClassName)
{
	FName FamilyName;
	UClass* OutClass = nullptr;
	if (FCortexGraphNodeContract::ResolveFamily(NodeClassName, FamilyName, OutClass))
	{
		return OutClass;
	}
	return nullptr;
}


FCortexCommandResult FCortexGraphNodeOps::DescribeNode(const TSharedPtr<FJsonObject>& Params)
{
	FString NodeClassName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("node_class"), NodeClassName) || NodeClassName.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: node_class"));
	}

	const FCortexNodeConstructionContract Contract = FCortexGraphNodeContract::Describe(NodeClassName);
	if (!Contract.bSupported)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Node class not supported: %s"), *NodeClassName));
	}

	TSharedPtr<FJsonObject> Data = Contract.ToJson();

	UClass* NodeClass = ResolveNodeClass(NodeClassName);
	if (NodeClass != nullptr)
	{
		Data->SetStringField(TEXT("canonical_node_class"), NodeClass->GetPathName());
	}

	FString AssetPath;
	const bool bHasAssetPath = Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty();

	const TSharedPtr<FJsonObject>* NodeParamsPtr = nullptr;
	const bool bHasParams = Params->TryGetObjectField(TEXT("params"), NodeParamsPtr) && NodeParamsPtr != nullptr;
	const TSharedPtr<FJsonObject> NodeParams = bHasParams ? *NodeParamsPtr : MakeShared<FJsonObject>();

	if (!bHasAssetPath)
	{
		Data->SetStringField(TEXT("validation_status"), TEXT("family_only"));

		if (bHasParams)
		{
			// Family-only probe without asset context: for static libraries or standalone nodes
			UBlueprint* ProbeOwner = NewObject<UBlueprint>(GetTransientPackage(), UBlueprint::StaticClass());
			ProbeOwner->AddToRoot();
			ProbeOwner->ParentClass = AActor::StaticClass();
			FKismetEditorUtilities::CompileBlueprint(ProbeOwner);
			if (TSharedPtr<FJsonObject> Pins = AllocateProbePins(ProbeOwner, NodeClassName, NodeParams, NodeClass))
			{
				const TArray<TSharedPtr<FJsonValue>>* PinArray = nullptr;
				if (Pins->TryGetArrayField(TEXT("pins"), PinArray) && PinArray != nullptr)
				{
					Data->SetArrayField(TEXT("expected_pins"), *PinArray);
					Data->SetBoolField(TEXT("pins_allocated"), true);
				}
			}
			ProbeOwner->RemoveFromRoot();
			ProbeOwner->MarkAsGarbage();
		}

		return FCortexCommandRouter::Success(Data);
	}

	// Contextual describe with asset_path
	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	if (Blueprint->ParentClass == nullptr || Blueprint->GeneratedClass == nullptr || Blueprint->Status == BS_BeingCreated)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("Blueprint class context is unready: missing ParentClass or GeneratedClass"));
	}

	// Validate target if provided
	const TSharedPtr<FJsonObject>* TargetObjPtr = nullptr;
	const bool bHasTarget = Params->TryGetObjectField(TEXT("target"), TargetObjPtr) && TargetObjPtr != nullptr && (*TargetObjPtr).IsValid();
	TSharedPtr<FJsonObject> ResolvedTargetObj;

	if (bHasTarget)
	{
		const TSharedPtr<FJsonObject>& TargetObj = *TargetObjPtr;
		FString TargetGuidStr;
		FString SubgraphPath;
		FString TargetKind;

		const TSharedPtr<FJsonObject>* GraphRefPtr = nullptr;
		if (TargetObj->TryGetObjectField(TEXT("graph_ref"), GraphRefPtr) && GraphRefPtr && (*GraphRefPtr).IsValid())
		{
			(*GraphRefPtr)->TryGetStringField(TEXT("graph_guid"), TargetGuidStr);
			(*GraphRefPtr)->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
			(*GraphRefPtr)->TryGetStringField(TEXT("graph_kind"), TargetKind);
		}
		else
		{
			TargetObj->TryGetStringField(TEXT("graph_guid"), TargetGuidStr);
			TargetObj->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
			TargetObj->TryGetStringField(TEXT("graph_kind"), TargetKind);
		}

		if (TargetGuidStr.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("target requires graph_guid or graph_ref with graph_guid"));
		}

		FGuid TargetGuid;
		if (!FGuid::Parse(TargetGuidStr, TargetGuid))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Invalid graph_guid format: '%s'"), *TargetGuidStr));
		}

		TArray<FCortexGraphEntry> Entries;
		EnumerateUserGraphs(Blueprint, Entries);

		UEdGraph* FoundGraph = nullptr;
		ECortexGraphKind FoundKind = ECortexGraphKind::Function;
		for (const FCortexGraphEntry& Entry : Entries)
		{
			if (Entry.Graph && Entry.Graph->GraphGuid == TargetGuid)
			{
				FoundGraph = Entry.Graph;
				FoundKind = Entry.Kind;
				break;
			}
		}

		if (FoundGraph == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::GraphNotFound,
				FString::Printf(TEXT("Graph with GUID %s not found on Blueprint"), *TargetGuidStr));
		}

		if (FoundKind == ECortexGraphKind::Delegate || !IsMutableGraphKind(FoundKind))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("Delegate signature graphs are read-only and cannot be targeted for authoring"));
		}

		ResolvedTargetObj = MakeShared<FJsonObject>();
		ResolvedTargetObj->SetStringField(TEXT("graph_guid"), FoundGraph->GraphGuid.ToString());
		ResolvedTargetObj->SetStringField(TEXT("graph_name"), FoundGraph->GetName());
		ResolvedTargetObj->SetStringField(TEXT("graph_kind"), GraphKindToString(FoundKind));
		ResolvedTargetObj->SetBoolField(TEXT("is_mutable"), IsMutableGraphKind(FoundKind));
		if (!SubgraphPath.IsEmpty())
		{
			ResolvedTargetObj->SetStringField(TEXT("subgraph_path"), SubgraphPath);
		}
	}

	// Validate node params against Blueprint context
	FCortexCommandResult ValidateError;
	if (!FCortexGraphNodeContract::Validate(NodeClassName, Blueprint, NodeParams, ValidateError))
	{
		return ValidateError;
	}

	// Populate resolved symbol if applicable
	FName FamilyName;
	UClass* DummyClass = nullptr;
	FCortexGraphNodeContract::ResolveFamily(NodeClassName, FamilyName, DummyClass);

	if (FamilyName == FName("CallFunction") || FamilyName == FName("Event"))
	{
		FCortexResolvedSymbol Symbol;
		FCortexCommandResult SymError;
		if (FCortexGraphSymbolResolver::ResolveFunction(Blueprint, NodeParams, Symbol, SymError))
		{
			Data->SetObjectField(TEXT("resolved_symbol"), Symbol.ToJson());
		}
	}
	else if (FamilyName == FName("VariableGet") || FamilyName == FName("VariableSet"))
	{
		const bool bIsWrite = (FamilyName == FName("VariableSet"));
		FCortexResolvedSymbol Symbol;
		FCortexCommandResult SymError;
		if (FCortexGraphSymbolResolver::ResolveProperty(Blueprint, NodeParams, bIsWrite, Symbol, SymError))
		{
			Data->SetObjectField(TEXT("resolved_symbol"), Symbol.ToJson());
		}
	}

	// Allocate probe pins in real context without mutating Blueprint or its dirty state
	const bool bWasDirty = Blueprint->GetOutermost()->IsDirty();
	if (TSharedPtr<FJsonObject> Pins = AllocateProbePins(Blueprint, NodeClassName, NodeParams, NodeClass))
	{
		const TArray<TSharedPtr<FJsonValue>>* PinArray = nullptr;
		if (Pins->TryGetArrayField(TEXT("pins"), PinArray) && PinArray != nullptr)
		{
			Data->SetArrayField(TEXT("expected_pins"), *PinArray);
			Data->SetBoolField(TEXT("pins_allocated"), true);
		}
	}

	// Guarantee zero mutation: dirty state must remain exactly as it was before
	if (Blueprint->GetOutermost()->IsDirty() != bWasDirty)
	{
		Blueprint->GetOutermost()->SetDirtyFlag(bWasDirty);
	}

	// Set validation_status
	if (bHasTarget)
	{
		Data->SetStringField(TEXT("validation_status"), TEXT("validated"));
	}
	else
	{
		Data->SetStringField(TEXT("validation_status"), TEXT("context_required"));
	}

	// Set context object
	TSharedPtr<FJsonObject> ContextObj = MakeShared<FJsonObject>();
	ContextObj->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	ContextObj->SetStringField(TEXT("self_class"),
		Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : (Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT("")));
	ContextObj->SetStringField(TEXT("parent_class"),
		Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	if (ResolvedTargetObj.IsValid())
	{
		ContextObj->SetObjectField(TEXT("target"), ResolvedTargetObj);
	}
	Data->SetObjectField(TEXT("context"), ContextObj);

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

TSharedRef<FJsonObject> FCortexGraphNodeOps::SerializePin(
	const UEdGraphPin* Pin,
	bool bDetailed,
	bool bCompact,
	const UBlueprint* ContextBlueprint)
{
	TSharedRef<FJsonObject> PinEntry = MakeShared<FJsonObject>();
	PinEntry->SetStringField(TEXT("name"), Pin->PinName.ToString());
	PinEntry->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
	PinEntry->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
	PinEntry->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
	PinEntry->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());

	FString SubCategoryObjectPath;
	if (Pin->PinType.PinSubCategoryObject.IsValid())
	{
		SubCategoryObjectPath = Pin->PinType.PinSubCategoryObject->GetPathName();
	}
	else if (Pin->PinType.PinSubCategory == UEdGraphSchema_K2::PSC_Self)
	{
		const UBlueprint* BP = ContextBlueprint;
		if (BP == nullptr && Pin->GetOwningNode() != nullptr)
		{
			BP = FBlueprintEditorUtils::FindBlueprintForNode(Pin->GetOwningNode());
		}
		if (BP != nullptr)
		{
			UClass* SelfClass = BP->GeneratedClass ? BP->GeneratedClass.Get() : (BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass.Get() : BP->ParentClass.Get());
			if (SelfClass != nullptr)
			{
				SubCategoryObjectPath = SelfClass->GetPathName();
			}
		}
	}
	PinEntry->SetStringField(TEXT("sub_category_object"), SubCategoryObjectPath);

	FString ContainerTypeStr = TEXT("none");
	switch (Pin->PinType.ContainerType)
	{
	case EPinContainerType::Array:
		ContainerTypeStr = TEXT("array");
		break;
	case EPinContainerType::Set:
		ContainerTypeStr = TEXT("set");
		break;
	case EPinContainerType::Map:
		ContainerTypeStr = TEXT("map");
		break;
	default:
		break;
	}
	PinEntry->SetStringField(TEXT("container_type"), ContainerTypeStr);

	if (Pin->PinType.ContainerType == EPinContainerType::Map || !Pin->PinType.PinValueType.TerminalCategory.IsNone())
	{
		TSharedRef<FJsonObject> TermObj = MakeShared<FJsonObject>();
		TermObj->SetStringField(TEXT("category"), Pin->PinType.PinValueType.TerminalCategory.ToString());
		TermObj->SetStringField(TEXT("subcategory"), Pin->PinType.PinValueType.TerminalSubCategory.ToString());
		TermObj->SetStringField(TEXT("sub_category_object"),
			Pin->PinType.PinValueType.TerminalSubCategoryObject.IsValid() ? Pin->PinType.PinValueType.TerminalSubCategoryObject->GetPathName() : TEXT(""));
		TermObj->SetBoolField(TEXT("is_const"), Pin->PinType.PinValueType.bTerminalIsConst);
		PinEntry->SetObjectField(TEXT("map_terminal_type"), TermObj);
	}

	PinEntry->SetBoolField(TEXT("is_reference"), (bool)Pin->PinType.bIsReference);
	PinEntry->SetBoolField(TEXT("is_const"), (bool)Pin->PinType.bIsConst);
	PinEntry->SetBoolField(TEXT("is_advanced"), (bool)Pin->bAdvancedView);
	PinEntry->SetBoolField(TEXT("is_hidden"), (bool)Pin->bHidden);

	if (Pin->Direction == EGPD_Input)
	{
		TSharedPtr<FJsonObject> DefaultDesc = MakeShared<FJsonObject>();
		if (Pin->DefaultObject != nullptr)
		{
			DefaultDesc->SetStringField(TEXT("kind"), FCortexGraphPinDefaults::ReferenceLiteralKind(*Pin));
			DefaultDesc->SetStringField(TEXT("path"), Pin->DefaultObject->GetPathName());
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
		{
			DefaultDesc->SetStringField(TEXT("kind"), TEXT("text"));
			if (!Pin->DefaultTextValue.IsEmpty())
			{
				DefaultDesc->SetObjectField(TEXT("value"), FCortexSerializer::TextToJson(Pin->DefaultTextValue));
			}
			else if (!Pin->DefaultValue.IsEmpty())
			{
				DefaultDesc->SetObjectField(TEXT("value"), FCortexSerializer::TextToJson(FText::FromString(Pin->DefaultValue)));
			}
			else
			{
				DefaultDesc->SetObjectField(TEXT("value"), FCortexSerializer::TextToJson(FText::GetEmpty()));
			}
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
		{
			if (!Pin->DefaultValue.IsEmpty())
			{
				DefaultDesc->SetStringField(TEXT("kind"), TEXT("bool"));
				DefaultDesc->SetBoolField(TEXT("value"), Pin->DefaultValue.ToBool());
			}
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int64 || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
		{
			if (!Pin->DefaultValue.IsEmpty())
			{
				DefaultDesc->SetStringField(TEXT("kind"), TEXT("int"));
				DefaultDesc->SetNumberField(TEXT("value"), FCString::Atoi64(*Pin->DefaultValue));
			}
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Float || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Double)
		{
			if (!Pin->DefaultValue.IsEmpty())
			{
				DefaultDesc->SetStringField(TEXT("kind"), TEXT("real"));
				DefaultDesc->SetNumberField(TEXT("value"), FCString::Atod(*Pin->DefaultValue));
			}
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String)
		{
			if (!Pin->DefaultValue.IsEmpty())
			{
				DefaultDesc->SetStringField(TEXT("kind"), TEXT("string"));
				DefaultDesc->SetStringField(TEXT("value"), Pin->DefaultValue);
			}
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
		{
			if (!Pin->DefaultValue.IsEmpty())
			{
				DefaultDesc->SetStringField(TEXT("kind"), TEXT("name"));
				DefaultDesc->SetStringField(TEXT("value"), Pin->DefaultValue);
			}
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Enum)
		{
			if (!Pin->DefaultValue.IsEmpty())
			{
				DefaultDesc->SetStringField(TEXT("kind"), TEXT("enum"));
				DefaultDesc->SetStringField(TEXT("value"), Pin->DefaultValue);
			}
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
		{
			if (Pin->DefaultValue.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				DefaultDesc->SetStringField(TEXT("kind"), TEXT("null"));
			}
			else if (!Pin->DefaultValue.IsEmpty())
			{
				DefaultDesc->SetStringField(TEXT("kind"), FCortexGraphPinDefaults::ReferenceLiteralKind(*Pin));
				DefaultDesc->SetStringField(TEXT("path"), Pin->DefaultValue);
			}
		}

		if (DefaultDesc->HasField(TEXT("kind")))
		{
			PinEntry->SetObjectField(TEXT("default_descriptor"), DefaultDesc);
		}
	}

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

TSharedRef<FJsonObject> FCortexGraphNodeOps::SerializeNode(const UEdGraphNode* Node, bool bIncludePins, bool bCompact)
{
	TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("node_id"), Node->GetName());
	// The canonical node identity, additive next to the unchanged name-based node_id, so a client can
	// reconcile the deterministic identities it planned by inspection alone.
	if (Node->NodeGuid.IsValid())
	{
		Entry->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
	}

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

	if (const UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node))
	{
		if (CompositeNode->BoundGraph)
		{
			Entry->SetStringField(TEXT("subgraph_name"), CompositeNode->BoundGraph->GetName());
		}
	}

	if (Node->GetClass() == UK2Node_Tunnel::StaticClass())
	{
		Entry->SetBoolField(TEXT("is_tunnel_boundary"), true);
	}

	if (bIncludePins)
	{
		TArray<TSharedPtr<FJsonValue>> PinsArray;
		UBlueprint* BP = Node ? FBlueprintEditorUtils::FindBlueprintForNode(Node) : nullptr;
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
			PinsArray.Add(MakeShared<FJsonValueObject>(SerializePin(Pin, true, bCompact, BP)));
		}
		Entry->SetArrayField(TEXT("pins"), PinsArray);
	}

	return Entry;
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
	if (!ValidateWritableGraphNodeBlueprintAssetPath(AssetPath, LoadError))
	{
		return LoadError;
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	UEdGraph* Graph = nullptr;
	if (!ResolveMutableNodeGraph(Blueprint, GraphName, Graph, LoadError))
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
	const bool bHasValue = Params.IsValid() && Params->TryGetStringField(TEXT("value"), Value);
	const bool bHasText = Params.IsValid() && Params->HasTypedField<EJson::Object>(TEXT("text"));

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		&& Params->TryGetStringField(TEXT("node_id"), NodeId)
		&& Params->TryGetStringField(TEXT("pin_name"), PinName)
		&& (bHasValue || bHasText);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, node_id, pin_name, and one of value or text")
		);
	}
	if (bHasValue && bHasText)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("graph.set_pin_value accepts either value or text, not both"));
	}
	if (Params.IsValid()
		&& Params->HasField(TEXT("expected_fingerprint"))
		&& !Params->HasTypedField<EJson::Object>(TEXT("expected_fingerprint")))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("expected_fingerprint must be an object when provided"));
	}
	if (bHasText && AssetPath.StartsWith(TEXT("__level_bp__:")))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::UnsupportedOperation,
			TEXT("Structured text pin mutation does not support level Blueprint targets in Phase 1"));
	}

	FCortexCommandResult LoadError;
	if (!ValidateWritableGraphNodeBlueprintAssetPath(AssetPath, LoadError))
	{
		return LoadError;
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	FString GraphKind;
	Params->TryGetStringField(TEXT("graph_kind"), GraphKind);
	FString OwningInterface;
	Params->TryGetStringField(TEXT("owning_interface"), OwningInterface);

	UEdGraph* Graph = nullptr;
	if (!ResolveMutableNodeGraphForSetPinValue(Blueprint, GraphName, GraphKind, OwningInterface, bHasText, Graph, LoadError))
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

	TSharedPtr<FJsonObject> FingerprintBefore = MakeObjectAssetFingerprint(
		Blueprint,
		GetTypeHash(static_cast<uint32>(Blueprint->Status))).ToJson();
	if (Params->HasTypedField<EJson::Object>(TEXT("expected_fingerprint")))
	{
		const TSharedPtr<FJsonObject> ExpectedFingerprint = Params->GetObjectField(TEXT("expected_fingerprint"));
		if (!FCortexBatchMutation::FingerprintsMatch(FingerprintBefore, ExpectedFingerprint))
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetObjectField(TEXT("current_fingerprint"), FingerprintBefore);
			return FCortexCommandRouter::Error(
				CortexErrorCodes::StalePrecondition,
				FString::Printf(TEXT("Expected fingerprint does not match current asset fingerprint for %s"), *AssetPath),
				Details);
		}
	}
	if (bHasText)
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Text)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Structured text can only be applied to FText pins: %s"), *PinName));
		}

		return ApplyTextPinValue(
			Blueprint,
			Graph,
			Node,
			Pin,
			Params->GetObjectField(TEXT("text")),
			AssetPath,
			GraphName,
			GraphKind,
			OwningInterface,
			SubgraphPath,
			NodeId,
			PinName,
			FingerprintBefore);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Set pin value %s.%s"), *NodeId, *PinName)
	));

	Graph->Modify();
	Node->Modify();

	// Set the default value
	// For class/object pins, try to resolve and set DefaultObject
	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
	{
		Pin->DefaultTextValue = FText::FromString(Value);
		Pin->DefaultValue.Empty();
		Pin->DefaultObject = nullptr;
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
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

TSharedPtr<FJsonObject> FCortexGraphNodeOps::BuildPinLocator(
	const FString& AssetPath,
	UEdGraph* Graph,
	UEdGraphNode* Node,
	const FString& GraphName,
	const FString& GraphKind,
	const FString& OwningInterface,
	const FString& SubgraphPath,
	const FString& NodeId,
	const FString& PinName)
{
	TSharedPtr<FJsonObject> Locator = MakeShared<FJsonObject>();
	Locator->SetStringField(TEXT("asset_path"), AssetPath);
	Locator->SetStringField(TEXT("graph_name"), GraphName.IsEmpty() && Graph != nullptr ? Graph->GetName() : GraphName);
	if (!GraphKind.IsEmpty())
	{
		Locator->SetStringField(TEXT("graph_kind"), GraphKind);
	}
	if (!OwningInterface.IsEmpty())
	{
		Locator->SetStringField(TEXT("owning_interface"), OwningInterface);
	}
	if (!SubgraphPath.IsEmpty())
	{
		Locator->SetStringField(TEXT("subgraph_path"), SubgraphPath);
	}
	Locator->SetStringField(TEXT("node_id"), NodeId);
	if (Node != nullptr)
	{
		Locator->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	}
	Locator->SetStringField(TEXT("pin_name"), PinName);
	return Locator;
}

FCortexCommandResult FCortexGraphNodeOps::ApplyTextPinValue(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	UEdGraphNode* Node,
	UEdGraphPin* Pin,
	const TSharedPtr<FJsonObject>& TextObject,
	const FString& AssetPath,
	const FString& GraphName,
	const FString& GraphKind,
	const FString& OwningInterface,
	const FString& SubgraphPath,
	const FString& NodeId,
	const FString& PinName,
	const TSharedPtr<FJsonObject>& FingerprintBefore)
{
	TArray<FString> TextErrors;
	TSharedPtr<FJsonObject> AppliedText;
	FText NewText;
	const FString EffectiveGraphName = GraphName.IsEmpty() && Graph != nullptr ? Graph->GetName() : GraphName;
	if (!FCortexSerializer::NormalizeTextDescriptor(
		MakeShared<FJsonValueObject>(TextObject),
		AppliedText,
		&NewText,
		TextErrors))
	{
		const FString ErrorMessage = FString::Join(TextErrors, TEXT("; "));
		const FString ErrorCode = ErrorMessage.Contains(TEXT("Unsupported FText source_kind"))
			? CortexErrorCodes::UnsupportedOperation
			: CortexErrorCodes::InvalidField;
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Set text pin value %s.%s"), *NodeId, *PinName)));

	Graph->Modify();
	Node->Modify();
	Pin->DefaultTextValue = NewText;
	Pin->DefaultValue.Empty();
	Pin->DefaultObject = nullptr;

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	const TSharedPtr<FJsonObject> VerifiedText = FCortexSerializer::TextToJson(Pin->DefaultTextValue);
	TArray<FString> CompareErrors;
	const bool bVerifiedInMemory = FCortexSerializer::TextDescriptorsEqual(
		MakeShared<FJsonValueObject>(AppliedText),
		MakeShared<FJsonValueObject>(VerifiedText),
		CompareErrors);
	if (!bVerifiedInMemory)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::VerificationFailed,
			CompareErrors.Num() > 0
				? FString::Join(CompareErrors, TEXT("; "))
				: TEXT("FText pin verification failed after mutation"));
	}

	UPackage* Package = Blueprint->GetOutermost();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bSaved = UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs);
	if (!bSaved)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::VerificationFailed,
			FString::Printf(TEXT("Failed to save Blueprint package %s after text pin mutation"), *Package->GetName()));
	}

	if (!ReloadBlueprintPackage(Package))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::VerificationFailed,
			FString::Printf(TEXT("Failed to reload Blueprint package %s after text pin mutation"), *Package->GetName()));
	}

	FCortexCommandResult ReloadError;
	UBlueprint* ReloadedBlueprint = LoadBlueprint(AssetPath, ReloadError);
	if (ReloadedBlueprint == nullptr)
	{
		return ReloadError;
	}

	UEdGraph* ReloadedGraph = nullptr;
	if (!ResolveMutableNodeGraphForSetPinValue(
			ReloadedBlueprint,
			EffectiveGraphName,
			GraphKind,
			OwningInterface,
			true,
			ReloadedGraph,
			ReloadError))
	{
		ReloadError.ErrorCode = CortexErrorCodes::LocatorDrift;
		ReloadError.ErrorMessage = TEXT("Failed to re-resolve graph after reload");
		return ReloadError;
	}
	if (!SubgraphPath.IsEmpty())
	{
		ReloadedGraph = ResolveSubgraph(ReloadedGraph, SubgraphPath, ReloadError);
		if (ReloadedGraph == nullptr)
		{
			ReloadError.ErrorCode = CortexErrorCodes::LocatorDrift;
			ReloadError.ErrorMessage = TEXT("Failed to re-resolve subgraph after reload");
			return ReloadError;
		}
	}

	UEdGraphNode* ReloadedNode = FindNode(ReloadedGraph, NodeId, ReloadError);
	if (ReloadedNode == nullptr)
	{
		ReloadError.ErrorCode = CortexErrorCodes::LocatorDrift;
		ReloadError.ErrorMessage = TEXT("Failed to re-resolve node after reload");
		return ReloadError;
	}

	UEdGraphPin* ReloadedPin = FindPin(ReloadedNode, PinName, ReloadError);
	if (ReloadedPin == nullptr)
	{
		ReloadError.ErrorCode = CortexErrorCodes::LocatorDrift;
		ReloadError.ErrorMessage = TEXT("Failed to re-resolve pin after reload");
		return ReloadError;
	}

	const TSharedPtr<FJsonObject> ReloadedText = FCortexSerializer::TextToJson(ReloadedPin->DefaultTextValue);
	CompareErrors.Reset();
	const bool bVerifiedReloaded = FCortexSerializer::TextDescriptorsEqual(
		MakeShared<FJsonValueObject>(AppliedText),
		MakeShared<FJsonValueObject>(ReloadedText),
		CompareErrors);
	if (!bVerifiedReloaded)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::VerificationFailed,
			CompareErrors.Num() > 0
				? FString::Join(CompareErrors, TEXT("; "))
				: TEXT("FText pin verification failed after reload"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("locator"), BuildPinLocator(
		AssetPath,
		ReloadedGraph,
		ReloadedNode,
		EffectiveGraphName,
		GraphKind,
		OwningInterface,
		SubgraphPath,
		NodeId,
		PinName));
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetStringField(TEXT("pin_name"), PinName);
	Data->SetObjectField(TEXT("applied_text"), AppliedText);
	Data->SetObjectField(TEXT("verified_text"), ReloadedText);
	Data->SetObjectField(TEXT("fingerprint_before"), FingerprintBefore);
	Data->SetObjectField(TEXT("fingerprint_after"), MakeObjectAssetFingerprint(
		ReloadedBlueprint,
		GetTypeHash(static_cast<uint32>(ReloadedBlueprint->Status))).ToJson());
	Data->SetBoolField(TEXT("compiled"), false);
	Data->SetBoolField(TEXT("saved"), true);
	Data->SetBoolField(TEXT("reloaded"), true);
	Data->SetBoolField(TEXT("verification_passed"), true);
	Data->SetBoolField(TEXT("locator_verified"), true);
	Data->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
	Data->SetBoolField(TEXT("success"), true);
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
	if (!ValidateWritableGraphNodeBlueprintAssetPath(AssetPath, LoadError))
	{
		return LoadError;
	}

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
		UEdGraph* Graph = nullptr;
		if (!ResolveMutableNodeGraph(Blueprint, GraphFilter, Graph, LoadError))
		{
			return LoadError;
		}

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
		TArray<FCortexGraphEntry> Entries;
		EnumerateUserGraphs(Blueprint, Entries);
		for (const FCortexGraphEntry& Entry : Entries)
		{
			if (Entry.Graph && IsMutableGraphKind(Entry.Kind))
			{
				Graphs.Add(Entry.Graph);
			}
		}
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
