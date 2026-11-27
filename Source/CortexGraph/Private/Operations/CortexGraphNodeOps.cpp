#include "Operations/CortexGraphNodeOps.h"
#include "Operations/CortexGraphNodeContract.h"
#include "Operations/CortexGraphPinDefaults.h"
#include "Operations/CortexGraphPropertyAccess.h"
#include "Operations/CortexGraphSymbolResolver.h"
#include "CortexAssetFingerprint.h"
#include "CortexBatchMutation.h"
#include "CortexGraphModule.h"
#include "CortexGraphNodeVariation.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "CortexSerializer.h"
#include "CortexGraphBindingStorage.h"
#include "CortexEditorUtils.h"
#include "CortexGraphLayoutOps.h"
#include "CortexBatchScope.h"
#include "Engine/Blueprint.h"
#include "Animation/AnimBlueprint.h"
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
#include "NodeFactory.h"
#include "Operations/CortexGraphOverrideEvents.h"
#include "SGraphNode.h"

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

enum class ECortexNodePlacementSide : uint8
{
	Left,
	Right,
	Above,
	Below
};

/** graph.add_node `placement`: the new node goes beside an anchor node's body. */
struct FCortexNodePlacementRequest
{
	bool bRequested = false;
	FString AnchorId;
	ECortexNodePlacementSide Side = ECortexNodePlacementSide::Right;
	int32 Gap = 80;
};

bool ParseNodePlacementRequest(const TSharedPtr<FJsonObject>& Params, FCortexNodePlacementRequest& OutRequest, FCortexCommandResult& OutError)
{
	OutRequest = FCortexNodePlacementRequest();
	if (!Params->HasField(TEXT("placement")))
	{
		return true;
	}

	const TSharedPtr<FJsonObject>* PlacementObject = nullptr;
	if (!Params->TryGetObjectField(TEXT("placement"), PlacementObject) || PlacementObject == nullptr || !PlacementObject->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("placement must be an object {relative_to, side, gap?}"));
		return false;
	}
	if (Params->HasField(TEXT("position")))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("placement and position are mutually exclusive"));
		return false;
	}

	if (!(*PlacementObject)->TryGetStringField(TEXT("relative_to"), OutRequest.AnchorId) || OutRequest.AnchorId.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("placement.relative_to is required: the node_id or node_guid of the anchor node"));
		return false;
	}

	FString SideText;
	(*PlacementObject)->TryGetStringField(TEXT("side"), SideText);
	if (SideText == TEXT("left"))
	{
		OutRequest.Side = ECortexNodePlacementSide::Left;
	}
	else if (SideText == TEXT("right"))
	{
		OutRequest.Side = ECortexNodePlacementSide::Right;
	}
	else if (SideText == TEXT("above"))
	{
		OutRequest.Side = ECortexNodePlacementSide::Above;
	}
	else if (SideText == TEXT("below"))
	{
		OutRequest.Side = ECortexNodePlacementSide::Below;
	}
	else
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("placement.side must be one of left, right, above, below (got '%s')"), *SideText));
		return false;
	}

	if ((*PlacementObject)->HasField(TEXT("gap")))
	{
		double GapValue = 0.0;
		if (!(*PlacementObject)->TryGetNumberField(TEXT("gap"), GapValue) || GapValue < 0.0 || GapValue > static_cast<double>(MAX_int32))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("placement.gap must be a number between 0 and 2147483647"));
			return false;
		}
		OutRequest.Gap = FMath::RoundToInt(GapValue);
	}

	OutRequest.bRequested = true;
	return true;
}

/** The anchor of a placement: a node of Graph addressed by node_id (object name) or node_guid. */
UEdGraphNode* FindPlacementAnchorNode(UEdGraph* Graph, const FString& AnchorId)
{
	FGuid AnchorGuid;
	const bool bIsGuid = FGuid::Parse(AnchorId, AnchorGuid);
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && (Node->GetName() == AnchorId || (bIsGuid && Node->NodeGuid == AnchorGuid)))
		{
			return Node;
		}
	}
	return nullptr;
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

/**
 * Resolve a reference literal of a legacy pin write to a live object. In-memory objects win; a disk
 * load only happens when the package is actually available, so a mistyped path cannot emit
 * SkipPackage warnings. An empty path or "None" is the null reference.
 */
UObject* ResolvePinReferenceObject(const FString& ReferencePath)
{
	const FString TrimmedPath = ReferencePath.TrimStartAndEnd();
	if (TrimmedPath.IsEmpty() || TrimmedPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		return nullptr;
	}

	if (UObject* InMemory = FindObject<UObject>(nullptr, *TrimmedPath))
	{
		return InMemory;
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(TrimmedPath);
	const bool bPackageAvailable = !PackageName.IsEmpty()
		&& (FindPackage(nullptr, *PackageName) != nullptr
			|| FPackageName::DoesPackageExist(PackageName)
			|| FPackageName::IsScriptPackage(PackageName));
	if (!bPackageAvailable)
	{
		return nullptr;
	}

	return LoadObject<UObject>(nullptr, *TrimmedPath);
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
	UEdGraphNode* ProbeNode = NewObject<UEdGraphNode>(ProbeGraph, FCortexGraphNodeContract::ResolveConstructionClass(NodeClass, NodeParams), NAME_None, RF_Transient);
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
			CortexErrorCodes::BlueprintNotFound,
			FString::Printf(TEXT("Blueprint not found: %s"), *NormalizedPath)
		);
		return nullptr;
	}

	// A package path (/Game/X/BP_A) names the asset like every other Blueprint command accepts it: its object is the package's short name.
	const FString ObjectPath = NormalizedPath.Contains(TEXT(".")) ? NormalizedPath : NormalizedPath + TEXT(".") + FPackageName::GetShortName(NormalizedPath);
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	if (Blueprint == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintNotFound,
			FString::Printf(TEXT("Blueprint not found: %s"), *NormalizedPath)
		);
	}
	return Blueprint;
}

void FCortexGraphNodeOps::EnumerateUserGraphs(const UBlueprint* Blueprint, TArray<FCortexGraphEntry>& OutEntries)
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

/** The Slate desired size of a node body, rounded up: the estimate auto-layout and placement both use. */
bool FCortexGraphNodeOps::EstimateNodeBodySize(UEdGraphNode* Node, FIntPoint& OutSize)
{
	const TSharedPtr<SGraphNode> Widget = FNodeFactory::CreateNodeWidget(Node);
	if (!Widget.IsValid())
	{
		return false;
	}
	Widget->SlatePrepass(1.0f);
	const FVector2D BodySize = Widget->GetDesiredSize();
	OutSize.X = FMath::Max(1, FMath::CeilToInt(BodySize.X));
	OutSize.Y = FMath::Max(1, FMath::CeilToInt(BodySize.Y));
	return true;
}

bool FCortexGraphNodeOps::ValidateWritableAssetPath(const FString& AssetPath, FCortexCommandResult& OutError)
{
	return ValidateWritableGraphNodeBlueprintAssetPath(AssetPath, OutError);
}

bool FCortexGraphNodeOps::ResolveMutableGraph(UBlueprint* Blueprint, const FString& GraphName, UEdGraph*& OutGraph, FCortexCommandResult& OutError)
{
	return ResolveMutableNodeGraph(Blueprint, GraphName, OutGraph, OutError);
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

	bool bIncludeSubgraphs = false;
	Params->TryGetBoolField(TEXT("include_subgraphs"), bIncludeSubgraphs);
	FString KindFilter;
	Params->TryGetStringField(TEXT("kind"), KindFilter);
	FString ParentGuidText;
	Params->TryGetStringField(TEXT("parent_graph_guid"), ParentGuidText);

	FGuid ParentGuidFilter;
	if (!ParentGuidText.IsEmpty() && !FGuid::Parse(ParentGuidText, ParentGuidFilter))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid parent_graph_guid format: '%s'"), *ParentGuidText)
		);
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	// A kind or parent filter addresses nested graphs, so either one lists them without a separate include_subgraphs.
	const bool bListNested = bIncludeSubgraphs || !KindFilter.IsEmpty() || !ParentGuidText.IsEmpty();

	TArray<FCortexGraphTreeEntry> Tree;
	FCortexGraphTree::Collect(Blueprint, Tree);

	const auto Matches = [&](const FCortexGraphTreeEntry& Entry)
	{
		return (KindFilter.IsEmpty() || Entry.Kind == KindFilter)
			&& (ParentGuidText.IsEmpty() || Entry.ParentGraphGuid == ParentGuidFilter);
	};

	// Root graphs first, then every nested graph in collection order.
	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	for (const FCortexGraphTreeEntry& Entry : Tree)
	{
		if (Entry.bRoot && Matches(Entry))
		{
			GraphsArray.Add(MakeShared<FJsonValueObject>(FCortexGraphTree::ToJson(Entry)));
		}
	}
	if (bListNested)
	{
		for (const FCortexGraphTreeEntry& Entry : Tree)
		{
			if (!Entry.bRoot && Matches(Entry))
			{
				GraphsArray.Add(MakeShared<FJsonValueObject>(FCortexGraphTree::ToJson(Entry)));
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("graphs"), GraphsArray);
	if (bListNested)
	{
		FCortexGraphTree::AppendTruncation(Tree, Data);
	}
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

namespace
{
	// Matches a node class filter against a node's class chain. A trailing '*' is a case-insensitive prefix match on
	// the runtime class name, used verbatim (runtime names carry no C++ prefix, but a prefix may legitimately begin
	// with 'U'); otherwise the filter names the node's class or any superclass, where a leading 'U' is optional.
	bool NodeClassMatches(const UClass* NodeClassPtr, const FString& Filter)
	{
		if (Filter.EndsWith(TEXT("*")))
		{
			return NodeClassPtr->GetName().StartsWith(Filter.LeftChop(1), ESearchCase::IgnoreCase);
		}

		const FString PatternNoPrefix = Filter.StartsWith(TEXT("U")) ? Filter.Mid(1) : Filter;
		for (const UClass* Class = NodeClassPtr; Class != nullptr; Class = Class->GetSuperClass())
		{
			const FString ClassName = Class->GetName();
			if (ClassName.Equals(Filter, ESearchCase::IgnoreCase)
				|| ClassName.Equals(PatternNoPrefix, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
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

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	// compact=true by default: omit node_class from results
	bool bCompact = true;
	Params->TryGetBoolField(TEXT("compact"), bCompact);

	// The whole graph tree (nested graphs included) unless graph_guid/graph_path/graph_name/subgraph_path scope the search
	// to one graph and the graphs nested below it.
	TArray<FCortexGraphTreeEntry> Scope;
	if (!FCortexGraphTree::ResolveReadScope(Blueprint, Params, Scope, LoadError))
	{
		return LoadError;
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	for (const FCortexGraphTreeEntry& GraphEntry : Scope)
	{
		UEdGraph* Graph = GraphEntry.Graph;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}

			// Apply filters
			if (!NodeClass.IsEmpty() && !NodeClassMatches(Node->GetClass(), NodeClass))
			{
				continue;
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
			Entry->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
			if (!GraphEntry.SubgraphPath.IsEmpty())
			{
				Entry->SetStringField(TEXT("subgraph_path"), GraphEntry.SubgraphPath);
			}
			if (const TSharedPtr<FJsonObject> PropertyAccess = FCortexGraphPropertyAccess::Describe(Node))
			{
				Entry->SetObjectField(TEXT("property_access"), PropertyAccess);
			}
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetNumberField(TEXT("count"), ResultsArray.Num());
	FCortexGraphTree::AppendTruncation(Scope, Data);
	return FCortexCommandRouter::Success(Data);
}

void FCortexGraphNodeOps::PlaceNewAnimNode(UEdGraphNode* Node)
{
	if (FCortexGraphBindingStorage::IsAnimNode(Node) && Node->GetTypedOuter<UAnimBlueprint>() != nullptr)
	{
		Node->PostPlacedNewNode();
	}
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

	FCortexNodePlacementRequest Placement;
	FCortexCommandResult PlacementError;
	if (!ParseNodePlacementRequest(Params, Placement, PlacementError))
	{
		return PlacementError;
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

	// add_node addresses its graph by graph_name (+ subgraph_path). A graph_guid is never silently ignored, which would add
	// the node to the default EventGraph: nested graphs are read-only (INVALID_OPERATION, as for apply_patch), and a root or
	// composite graph is refused with the name-based addressing to use instead.
	FString GraphGuidText;
	Params->TryGetStringField(TEXT("graph_guid"), GraphGuidText);
	if (!GraphGuidText.IsEmpty())
	{
		TSharedPtr<FJsonObject> IdentityParams = MakeShared<FJsonObject>();
		IdentityParams->SetStringField(TEXT("graph_guid"), GraphGuidText);
		UEdGraph* IdentifiedGraph = nullptr;
		FCortexGraphTreeEntry Identified;
		if (!FCortexGraphTree::ResolveReadTarget(Blueprint, IdentityParams, IdentifiedGraph, &Identified, LoadError))
		{
			return LoadError;
		}
		if (!Identified.bRoot && Identified.SubgraphPath.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Graph %s (%s) is a nested graph and is read-only; graph.add_node cannot add nodes to it"),
					*Identified.Path, *Identified.Kind));
		}
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("graph.add_node does not accept graph_guid; address graph '%s' with graph_name%s"),
				*Identified.RootGraphName, Identified.SubgraphPath.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" and subgraph_path '%s'"), *Identified.SubgraphPath)));
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

	// The anchor is resolved (and, for right/below, measured) before anything is created, so a bad anchor adds nothing.
	UEdGraphNode* PlacementAnchor = nullptr;
	FIntPoint PlacementAnchorSize(0, 0);

	// A node body is never wider or taller than this, so left/above placement can be range-checked before the node exists
	// (creating a composite node also creates its bound graph, which removing the node would leave behind).
	constexpr int32 PlacementMaxBodyExtent = 1000000;
	// The placed coordinates are computed in 64 bits: an int32 sum can wrap to the wrong side of the anchor.
	auto TryComputePlacement = [&](const FIntPoint& NewNodeSize, int32& OutX, int32& OutY) -> bool
	{
		int64 PlacedX = PlacementAnchor->NodePosX;
		int64 PlacedY = PlacementAnchor->NodePosY;
		switch (Placement.Side)
		{
		case ECortexNodePlacementSide::Left:
			PlacedX -= static_cast<int64>(Placement.Gap) + NewNodeSize.X;
			break;
		case ECortexNodePlacementSide::Right:
			PlacedX += static_cast<int64>(PlacementAnchorSize.X) + Placement.Gap;
			break;
		case ECortexNodePlacementSide::Above:
			PlacedY -= static_cast<int64>(Placement.Gap) + NewNodeSize.Y;
			break;
		case ECortexNodePlacementSide::Below:
			PlacedY += static_cast<int64>(PlacementAnchorSize.Y) + Placement.Gap;
			break;
		}
		if (PlacedX < MIN_int32 || PlacedX > MAX_int32 || PlacedY < MIN_int32 || PlacedY > MAX_int32)
		{
			return false;
		}
		OutX = static_cast<int32>(PlacedX);
		OutY = static_cast<int32>(PlacedY);
		return true;
	};
	const FString PlacementRangeMessage = TEXT("placement puts the node outside the supported coordinate range; use a smaller gap");
	if (Placement.bRequested)
	{
		PlacementAnchor = FindPlacementAnchorNode(Graph, Placement.AnchorId);
		if (PlacementAnchor == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::NodeNotFound,
				FString::Printf(TEXT("placement.relative_to node not found in the resolved graph: %s"), *Placement.AnchorId));
		}
		if (Placement.Side == ECortexNodePlacementSide::Right || Placement.Side == ECortexNodePlacementSide::Below)
		{
			if (!FCortexGraphNodeOps::EstimateNodeBodySize(PlacementAnchor, PlacementAnchorSize))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("Cannot measure graph node: %s"), *PlacementAnchor->GetName()));
			}
		}
		// Refused before anything is created: right/below depend only on the anchor, left/above are checked against the
		// maximum body extent, so the later positioning cannot leave the int32 range.
		int32 CheckX = 0;
		int32 CheckY = 0;
		if (!TryComputePlacement(FIntPoint(PlacementMaxBodyExtent, PlacementMaxBodyExtent), CheckX, CheckY))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, PlacementRangeMessage);
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
	if (FCortexGraphPropertyAccess::IsPropertyAccessNode(NodeClass->GetDefaultObject<UEdGraphNode>())
		&& !FCortexGraphPropertyAccess::ValidateGraph(Blueprint, Graph, ContractError))
	{
		return ContractError;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Add node %s"), *NodeClassName)
	));

	Graph->Modify();

	UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, FCortexGraphNodeContract::ResolveConstructionClass(NodeClass, NodeConstructionParams));
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

	FCortexGraphNodeOps::PlaceNewAnimNode(NewNode);

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

	if (Placement.bRequested)
	{
		// The new node's body is measured for left/above; when no widget can be built a default body is assumed. Either way
		// the size is capped at the extent the range check above used, so the position below is always in range.
		FIntPoint NewNodeSize(0, 0);
		if (Placement.Side == ECortexNodePlacementSide::Left || Placement.Side == ECortexNodePlacementSide::Above)
		{
			if (!FCortexGraphNodeOps::EstimateNodeBodySize(NewNode, NewNodeSize))
			{
				NewNodeSize = FIntPoint(200, 100);
			}
			NewNodeSize.X = FMath::Min(NewNodeSize.X, PlacementMaxBodyExtent);
			NewNodeSize.Y = FMath::Min(NewNodeSize.Y, PlacementMaxBodyExtent);
		}
		int32 PlacedX = 0;
		int32 PlacedY = 0;
		if (TryComputePlacement(NewNodeSize, PlacedX, PlacedY))
		{
			NewNode->NodePosX = PlacedX;
			NewNode->NodePosY = PlacedY;
		}
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
	{
		TSharedRef<FJsonObject> FinalPosition = MakeShared<FJsonObject>();
		FinalPosition->SetNumberField(TEXT("x"), NewNode->NodePosX);
		FinalPosition->SetNumberField(TEXT("y"), NewNode->NodePosY);
		Data->SetObjectField(TEXT("position"), FinalPosition);
	}
	if (const TSharedPtr<FJsonObject> PropertyAccess = FCortexGraphPropertyAccess::Describe(NewNode))
	{
		Data->SetObjectField(TEXT("property_access"), PropertyAccess);
	}

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

		// A Property Access path only resolves from a real Anim Blueprint class, so it is never probed without one.
		if (bHasParams && !FCortexGraphPropertyAccess::IsPropertyAccessNode(NodeClass ? NodeClass->GetDefaultObject<UEdGraphNode>() : nullptr))
		{
			// Family-only probe without asset context: for static libraries or standalone nodes
			UBlueprint* ProbeOwner = NewObject<UBlueprint>(GetTransientPackage(), UBlueprint::StaticClass());
			ProbeOwner->AddToRoot();
			ProbeOwner->ParentClass = AActor::StaticClass();
			FKismetEditorUtilities::CompileBlueprint(ProbeOwner, EBlueprintCompileOptions::SkipSave);
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
	const UEdGraph* ResolvedTargetGraph = nullptr;

	if (bHasTarget)
	{
		const TSharedPtr<FJsonObject>& TargetObj = *TargetObjPtr;
		FString TargetGuidStr;
		FString TargetPathStr;
		FString SubgraphPath;
		FString TargetKind;

		const TSharedPtr<FJsonObject>* GraphRefPtr = nullptr;
		if (TargetObj->TryGetObjectField(TEXT("graph_ref"), GraphRefPtr) && GraphRefPtr && (*GraphRefPtr).IsValid())
		{
			(*GraphRefPtr)->TryGetStringField(TEXT("graph_guid"), TargetGuidStr);
			(*GraphRefPtr)->TryGetStringField(TEXT("graph_path"), TargetPathStr);
			(*GraphRefPtr)->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
			(*GraphRefPtr)->TryGetStringField(TEXT("graph_kind"), TargetKind);
		}
		else
		{
			TargetObj->TryGetStringField(TEXT("graph_guid"), TargetGuidStr);
			TargetObj->TryGetStringField(TEXT("graph_path"), TargetPathStr);
			TargetObj->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
			TargetObj->TryGetStringField(TEXT("graph_kind"), TargetKind);
		}

		if (TargetGuidStr.IsEmpty() && TargetPathStr.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("target requires graph_guid or graph_path, or graph_ref with one of them"));
		}

		FGuid TargetGuid;
		if (!TargetGuidStr.IsEmpty() && !FGuid::Parse(TargetGuidStr, TargetGuid))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Invalid graph_guid format: '%s'"), *TargetGuidStr));
		}

		UEdGraph* FoundGraph = nullptr;
		ECortexGraphKind FoundKind = ECortexGraphKind::Function;
		if (TargetPathStr.IsEmpty())
		{
			TArray<FCortexGraphEntry> Entries;
			EnumerateUserGraphs(Blueprint, Entries);
			for (const FCortexGraphEntry& Entry : Entries)
			{
				if (Entry.Graph && Entry.Graph->GraphGuid == TargetGuid)
				{
					FoundGraph = Entry.Graph;
					FoundKind = Entry.Kind;
					break;
				}
			}
		}

		if (FoundGraph == nullptr)
		{
			// Not a root graph chosen by GUID: resolve through the shared tree rules. A nested graph (composite body,
			// Animation Blueprint state, transition or conduit graph) resolves read-only; graph_guid, graph_path and
			// subgraph_path must agree, a shared GUID is never guessed (graph_path selects), and graph_kind must match.
			TArray<FCortexGraphTreeEntry> Tree;
			FCortexGraphTree::Collect(Blueprint, Tree);
			TSharedPtr<FJsonObject> IdentityParams = MakeShared<FJsonObject>();
			if (!TargetGuidStr.IsEmpty())
			{
				IdentityParams->SetStringField(TEXT("graph_guid"), TargetGuidStr);
			}
			if (!TargetPathStr.IsEmpty())
			{
				IdentityParams->SetStringField(TEXT("graph_path"), TargetPathStr);
			}
			if (!SubgraphPath.IsEmpty())
			{
				IdentityParams->SetStringField(TEXT("subgraph_path"), SubgraphPath);
			}
			FCortexCommandResult ResolveError;
			const FCortexGraphTreeEntry* Resolved = FCortexGraphTree::ResolveInTree(Tree, IdentityParams, ResolveError);
			if (Resolved == nullptr)
			{
				return ResolveError;
			}

			if (Resolved->bRoot)
			{
				FoundGraph = Resolved->Graph;
				FoundKind = Resolved->RootKind;
			}
			else
			{
				if (!TargetKind.IsEmpty() && TargetKind != Resolved->Kind)
				{
					return FCortexCommandRouter::Error(
						CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("Graph kind mismatch: specified '%s' but graph is '%s'"), *TargetKind, *Resolved->Kind));
				}

				ResolvedTargetGraph = Resolved->Graph;
				ResolvedTargetObj = MakeShared<FJsonObject>();
				ResolvedTargetObj->SetStringField(TEXT("graph_guid"), Resolved->Graph->GraphGuid.ToString());
				ResolvedTargetObj->SetStringField(TEXT("graph_name"), Resolved->Graph->GetName());
				ResolvedTargetObj->SetStringField(TEXT("graph_kind"), Resolved->Kind);
				ResolvedTargetObj->SetBoolField(TEXT("is_mutable"), false);
				ResolvedTargetObj->SetStringField(TEXT("graph_path"), Resolved->Path);
				if (!Resolved->SubgraphPath.IsEmpty())
				{
					ResolvedTargetObj->SetStringField(TEXT("subgraph_path"), Resolved->SubgraphPath);
				}
			}
		}

		if (FoundGraph != nullptr)
		{
			ResolvedTargetGraph = FoundGraph;
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
	}

	// Validate node params against Blueprint context
	FCortexCommandResult ValidateError;
	if (!FCortexGraphNodeContract::Validate(NodeClassName, Blueprint, NodeParams, ValidateError))
	{
		return ValidateError;
	}
	if (FCortexGraphPropertyAccess::IsPropertyAccessNode(NodeClass ? NodeClass->GetDefaultObject<UEdGraphNode>() : nullptr)
		&& !FCortexGraphPropertyAccess::ValidateGraph(Blueprint, ResolvedTargetGraph, ValidateError))
	{
		return ValidateError;
	}

	// Populate resolved symbol if applicable
	FName FamilyName;
	UClass* DummyClass = nullptr;
	FCortexGraphNodeContract::ResolveFamily(NodeClassName, FamilyName, DummyClass);

	if (FamilyName == FName("CallFunction") || FamilyName == FName("Message") || FamilyName == FName("Event"))
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
	// Compact mode omits default-valued fields (issue #225): category duplicates type, and an
	// empty subcategory carries no information. Verbose mode keeps every field.
	if (!bCompact)
	{
		PinEntry->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		PinEntry->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
	}
	else if (!Pin->PinType.PinSubCategory.IsNone())
	{
		PinEntry->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
	}

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
	if (!bCompact || !SubCategoryObjectPath.IsEmpty())
	{
		PinEntry->SetStringField(TEXT("sub_category_object"), SubCategoryObjectPath);
	}

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
	if (!bCompact || ContainerTypeStr != TEXT("none"))
	{
		PinEntry->SetStringField(TEXT("container_type"), ContainerTypeStr);
	}

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

	if (!bCompact || Pin->PinType.bIsReference)
	{
		PinEntry->SetBoolField(TEXT("is_reference"), (bool)Pin->PinType.bIsReference);
	}
	if (!bCompact || Pin->PinType.bIsConst)
	{
		PinEntry->SetBoolField(TEXT("is_const"), (bool)Pin->PinType.bIsConst);
	}
	if (!bCompact || Pin->bAdvancedView)
	{
		PinEntry->SetBoolField(TEXT("is_advanced"), (bool)Pin->bAdvancedView);
	}
	if (!bCompact || Pin->bHidden)
	{
		PinEntry->SetBoolField(TEXT("is_hidden"), (bool)Pin->bHidden);
	}

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
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && Pin->PinType.PinSubCategoryObject.IsValid())
		{
			if (!Pin->DefaultValue.IsEmpty())
			{
				DefaultDesc->SetStringField(TEXT("kind"), TEXT("enum"));
				DefaultDesc->SetStringField(TEXT("value"), Pin->DefaultValue);
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

	if (const TSharedPtr<FJsonObject> PropertyAccess = FCortexGraphPropertyAccess::Describe(Node))
	{
		Entry->SetObjectField(TEXT("property_access"), PropertyAccess);
	}

	// The bound function of a Create Event node is not part of its title ("Create Event"), so it is
	// published as a field; the scope class is the class the function is looked up on.
	if (const UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(Node))
	{
		Entry->SetStringField(TEXT("function_name"), CreateDelegate->GetFunctionName().ToString());
		const UClass* ScopeClass = CreateDelegate->GetScopeClass();
		Entry->SetStringField(TEXT("scope_class"), ScopeClass ? ScopeClass->GetPathName() : FString());
	}

	// A Variable Get node is pure, or executes as a branch / validated-object check; the title and
	// the pure-looking class name do not tell which.
	if (Cast<UK2Node_VariableGet>(Node))
	{
		Entry->SetStringField(TEXT("variation"), FCortexGraphNodeVariation::GetVariableGetVariation(Node));
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

FCortexCommandResult FCortexGraphNodeOps::RemoveNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	const bool bHasNodeIds = Params->HasField(TEXT("node_ids"));
	const bool bHasKeepIds = Params->HasField(TEXT("keep_node_ids"));
	if (bHasNodeIds == bHasKeepIds)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Exactly one of node_ids (remove these nodes) or keep_node_ids (remove every other node) is required"));
	}

	const TCHAR* const IdField = bHasNodeIds ? TEXT("node_ids") : TEXT("keep_node_ids");
	const TArray<TSharedPtr<FJsonValue>>* IdValues = nullptr;
	if (!Params->TryGetArrayField(IdField, IdValues) || IdValues == nullptr || IdValues->Num() == 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be a non-empty array of node identifiers"), IdField));
	}
	TArray<FString> RequestedIds;
	for (const TSharedPtr<FJsonValue>& Value : *IdValues)
	{
		FString Id;
		if (!Value.IsValid() || !Value->TryGetString(Id) || Id.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s entries must be non-empty node identifier strings"), IdField));
		}
		RequestedIds.AddUnique(Id);
	}

	bool bDryRun = false;
	if (Params->HasField(TEXT("dry_run")) && !Params->TryGetBoolField(TEXT("dry_run"), bDryRun))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("dry_run must be a boolean"));
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

	// A node is addressed by its node_id (object name) or by its node_guid. Every identifier must resolve
	// before anything is removed, so a typo can never turn a keep set into a larger removal.
	TSet<const UEdGraphNode*> Selected;
	TArray<FString> Missing;
	for (const FString& Id : RequestedIds)
	{
		FGuid IdGuid;
		const bool bIsGuid = FGuid::Parse(Id, IdGuid);
		const UEdGraphNode* Match = nullptr;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && (Node->GetName() == Id || (bIsGuid && Node->NodeGuid == IdGuid)))
			{
				Match = Node;
				break;
			}
		}
		if (Match == nullptr)
		{
			Missing.Add(Id);
			continue;
		}
		Selected.Add(Match);
	}
	if (Missing.Num() > 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NodeNotFound,
			FString::Printf(TEXT("Node(s) not found in the resolved graph: %s"), *FString::Join(Missing, TEXT(", "))));
	}

	TArray<UEdGraphNode*> ToRemove;
	TArray<TSharedPtr<FJsonValue>> KeptJson;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node == nullptr || Selected.Contains(Node) != bHasNodeIds)
		{
			continue;
		}
		// Terminators own the graph's signature and are never removed; the same goes for any node the editor
		// itself refuses to delete.
		FString KeptReason;
		if (Node->IsA<UK2Node_FunctionEntry>())
		{
			KeptReason = TEXT("function entry node");
		}
		else if (Node->IsA<UK2Node_FunctionResult>())
		{
			KeptReason = TEXT("function result node");
		}
		else if (!Node->CanUserDeleteNode())
		{
			KeptReason = TEXT("node cannot be deleted");
		}
		if (!KeptReason.IsEmpty())
		{
			TSharedPtr<FJsonObject> Kept = MakeShared<FJsonObject>();
			Kept->SetStringField(TEXT("node_id"), Node->GetName());
			Kept->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
			Kept->SetStringField(TEXT("reason"), KeptReason);
			KeptJson.Add(MakeShared<FJsonValueObject>(Kept));
			continue;
		}
		ToRemove.Add(Node);
	}

	TArray<TSharedPtr<FJsonValue>> RemovedJson;
	int32 DisconnectedPins = 0;
	for (const UEdGraphNode* Node : ToRemove)
	{
		RemovedJson.Add(MakeShared<FJsonValueString>(Node->GetName()));
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && Pin->LinkedTo.Num() > 0)
			{
				++DisconnectedPins;
			}
		}
	}
	const int32 RetainedCount = Graph->Nodes.Num() - ToRemove.Num();
	// Informational pre-flight, computed before the removal while every call site still exists; never blocks it.
	const TArray<TSharedPtr<FJsonValue>> OverrideEventsJson = FCortexGraphOverrideEvents::Describe(Blueprint, ToRemove);

	if (!bDryRun && ToRemove.Num() > 0)
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex:Remove %d nodes"), ToRemove.Num())));
		Graph->Modify();
		for (UEdGraphNode* Node : ToRemove)
		{
			FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
		}
		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		UE_LOG(LogCortexGraph, Log, TEXT("Removed %d nodes from graph %s (%d pins disconnected)"),
			ToRemove.Num(), *Graph->GetName(), DisconnectedPins);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("graph_name"), Graph->GetName());
	Data->SetStringField(TEXT("mode"), bHasNodeIds ? TEXT("node_ids") : TEXT("keep_node_ids"));
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetArrayField(TEXT("removed_node_ids"), RemovedJson);
	Data->SetNumberField(TEXT("removed_count"), ToRemove.Num());
	Data->SetArrayField(TEXT("kept"), KeptJson);
	Data->SetNumberField(TEXT("remaining_node_count"), RetainedCount);
	Data->SetNumberField(TEXT("disconnected_pins"), DisconnectedPins);
	Data->SetArrayField(TEXT("override_events"), OverrideEventsJson);
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

	// The engine schema owns pin-default validity: resolve the candidate exactly as it would be
	// written, let the schema refuse it, and only then touch the pin. A refusal changes nothing.
	FString NewDefaultValue;
	UObject* NewDefaultObject = nullptr;
	FText NewDefaultText;

	const FName PinCategory = Pin->PinType.PinCategory;
	const bool bNullReference = Value.IsEmpty()
		|| Value.Equals(TEXT("None"), ESearchCase::IgnoreCase);

	if (PinCategory == UEdGraphSchema_K2::PC_Text)
	{
		NewDefaultText = FText::FromString(Value);
	}
	else if (PinCategory == UEdGraphSchema_K2::PC_Class || PinCategory == UEdGraphSchema_K2::PC_SoftClass)
	{
		UClass* ResolvedClass = nullptr;
		if (!bNullReference)
		{
			FCortexCommandResult ResolveError;
			if (!FCortexGraphSymbolResolver::ResolveClass(Value, ResolvedClass, ResolveError) || ResolvedClass == nullptr)
			{
				const FString Reason = ResolveError.ErrorMessage.IsEmpty()
					? FString::Printf(TEXT("'%s' did not resolve"), *Value)
					: ResolveError.ErrorMessage;
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidValue,
					FString::Printf(TEXT("Pin '%s' needs a resolvable class: %s"), *PinName, *Reason));
			}
		}

		if (PinCategory == UEdGraphSchema_K2::PC_SoftClass)
		{
			// Soft references stay soft: the path lives in DefaultValue, never in DefaultObject.
			NewDefaultValue = ResolvedClass != nullptr ? ResolvedClass->GetPathName() : FString();
		}
		else
		{
			NewDefaultObject = ResolvedClass;
		}
	}
	else if (PinCategory == UEdGraphSchema_K2::PC_Object
		|| PinCategory == UEdGraphSchema_K2::PC_Interface
		|| PinCategory == UEdGraphSchema_K2::PC_SoftObject)
	{
		UObject* ResolvedObject = ResolvePinReferenceObject(Value);
		if (ResolvedObject == nullptr && !bNullReference)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidValue,
				FString::Printf(TEXT("Pin '%s' needs a resolvable object path, but '%s' did not resolve"), *PinName, *Value));
		}

		if (PinCategory == UEdGraphSchema_K2::PC_SoftObject)
		{
			// Soft references stay soft: the path lives in DefaultValue, never in DefaultObject.
			NewDefaultValue = ResolvedObject != nullptr ? ResolvedObject->GetPathName() : FString();
		}
		else
		{
			NewDefaultObject = ResolvedObject;
		}
	}
	else
	{
		NewDefaultValue = Value;
	}

	if (const UEdGraphSchema* Schema = Graph->GetSchema())
	{
		const FString ValidationMessage =
			Schema->IsPinDefaultValid(Pin, NewDefaultValue, NewDefaultObject, NewDefaultText);
		if (!ValidationMessage.IsEmpty())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, ValidationMessage);
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Set pin value %s.%s"), *NodeId, *PinName)
	));

	Graph->Modify();
	Node->Modify();

	Pin->DefaultValue = NewDefaultValue;
	Pin->DefaultObject = NewDefaultObject;
	if (PinCategory == UEdGraphSchema_K2::PC_Text)
	{
		Pin->DefaultTextValue = NewDefaultText;
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

	// The engine schema owns the FText default representation; a refusal leaves the pin untouched.
	if (const UEdGraphSchema* Schema = Graph->GetSchema())
	{
		const FString ValidationMessage = Schema->IsPinDefaultValid(Pin, FString(), nullptr, NewText);
		if (!ValidationMessage.IsEmpty())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, ValidationMessage);
		}
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
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs);
	if (!bSaved)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SaveFailed,
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
	if (!SubgraphPath.IsEmpty() && GraphFilter.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("subgraph_path requires graph_name"));
	}

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
	int32 ChangedNodeCount = 0;

	TUniquePtr<FScopedTransaction> Transaction;

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

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				bool bIsExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
				if (Pin->Direction == EGPD_Input)
				{
					if (bIsExec) bHasExecInput = true;
				}
				else
				{
					if (bIsExec) bHasExecOutput = true;
				}
			}

			LN.bIsEntryPoint = (!bHasExecInput && bHasExecOutput);
			LN.bIsExecNode = (bHasExecInput || bHasExecOutput);
			FIntPoint BodyEstimate(1, 1);
			if (!FCortexGraphNodeOps::EstimateNodeBodySize(Node, BodyEstimate))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("Cannot measure graph node: %s"), *Node->GetName()));
			}
			LN.Width = BodyEstimate.X;
			LN.Height = BodyEstimate.Y;

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

		bool bGraphChanged = false;
		for (const auto& Pair : LayoutResult.Positions)
		{
			UEdGraphNode** NodePtr = IdToNode.Find(Pair.Key);
			if (NodePtr && *NodePtr &&
				((*NodePtr)->NodePosX != Pair.Value.X || (*NodePtr)->NodePosY != Pair.Value.Y))
			{
				if (!bGraphChanged)
				{
					if (!Transaction && !FCortexCommandRouter::IsInBatch())
					{
						Transaction = MakeUnique<FScopedTransaction>(
							FText::FromString(TEXT("Cortex: Auto-Layout Blueprint Graphs")));
					}
					Blueprint->Modify();
					Graph->Modify();
					bGraphChanged = true;
				}
				(*NodePtr)->Modify();
				(*NodePtr)->NodePosX = Pair.Value.X;
				(*NodePtr)->NodePosY = Pair.Value.Y;
				++ChangedNodeCount;
			}
		}

		if (bGraphChanged && FCortexCommandRouter::IsInBatch())
		{
			FString GraphKey = FString::Printf(TEXT("graph.notify.%s"), *Graph->GetPathName());
			FCortexBatchScope::AddCleanupAction(GraphKey,
				[WeakGraph = TWeakObjectPtr<UEdGraph>(Graph)]()
				{
					if (UEdGraph* G = WeakGraph.Get()) G->NotifyGraphChanged();
				});
		}
		else if (bGraphChanged)
		{
			Graph->NotifyGraphChanged();
		}

		TotalNodesProcessed += LayoutResult.Positions.Num();
	}

	if (ChangedNodeCount > 0)
	{
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
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("node_count"), TotalNodesProcessed);
	Data->SetNumberField(TEXT("graphs_processed"), Graphs.Num());
	Data->SetNumberField(TEXT("changed_node_count"), ChangedNodeCount);
	Data->SetBoolField(TEXT("unchanged"), ChangedNodeCount == 0);

	UE_LOG(LogCortexGraph, Log, TEXT("Auto-layout completed: %d nodes across %d graphs in %s"),
		TotalNodesProcessed, Graphs.Num(), *AssetPath);

	return FCortexCommandRouter::Success(Data);
}
