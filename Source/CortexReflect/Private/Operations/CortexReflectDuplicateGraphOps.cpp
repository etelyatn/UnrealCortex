#include "Operations/CortexReflectDuplicateGraphOps.h"

#include "Algo/Sort.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "CortexEditorUtils.h"
#include "CortexReflectModule.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/MemberReference.h"
#include "IO/IoHash.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_StructOperation.h"
#include "K2Node_Variable.h"
#include "Modules/ModuleManager.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"

namespace CortexReflectDuplicateGraphPrivate
{
	constexpr int32 DefaultMinNodes = 5;
	constexpr double DefaultSimilarity = 0.9;

	namespace UnitKind
	{
		const TCHAR* const Function = TEXT("function");
		const TCHAR* const Macro = TEXT("macro");
		const TCHAR* const Event = TEXT("event");
		const TCHAR* const WidgetTree = TEXT("widget_tree");
	}

	struct FRequest
	{
		TArray<FString> Prefixes;
		int32 MinNodes = DefaultMinNodes;
		double Similarity = DefaultSimilarity;
	};

	bool IsPlainFolderPath(const FString& Path)
	{
		return Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT("..")) && !Path.Contains(TEXT("//")) && !Path.Contains(TEXT("."));
	}

	/** Validates every parameter before any asset is touched. Returns an error result or an empty optional. */
	TOptional<FCortexCommandResult> ParseRequest(const TSharedPtr<FJsonObject>& Params, FRequest& OutRequest)
	{
		if (!Params.IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
		}

		const TArray<TSharedPtr<FJsonValue>>* PrefixValues = nullptr;
		if (!Params->TryGetArrayField(TEXT("path_prefixes"), PrefixValues) || PrefixValues->Num() == 0)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				TEXT("path_prefixes must be a non-empty array of mounted folder paths"));
		}
		for (const TSharedPtr<FJsonValue>& Value : *PrefixValues)
		{
			FString Prefix;
			if (!Value.IsValid() || !Value->TryGetString(Prefix) || Prefix.IsEmpty())
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					TEXT("path_prefixes must contain only non-empty folder path strings"));
			}
			Prefix = FCortexEditorUtils::NormalizeMountedContentPath(Prefix);
			while (Prefix.Len() > 1 && Prefix.EndsWith(TEXT("/")))
			{
				Prefix.LeftChopInline(1);
			}
			if (!IsPlainFolderPath(Prefix))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("path_prefixes entry is not a valid mounted folder path: %s"), *Prefix));
			}
			OutRequest.Prefixes.AddUnique(MoveTemp(Prefix));
		}

		if (Params->HasField(TEXT("min_nodes")))
		{
			double MinNodes = 0.0;
			if (!Params->TryGetNumberField(TEXT("min_nodes"), MinNodes) || MinNodes < 1.0 || MinNodes > MAX_int32
				|| FMath::FloorToDouble(MinNodes) != MinNodes)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("min_nodes must be an integer greater than 0"));
			}
			OutRequest.MinNodes = static_cast<int32>(MinNodes);
		}

		if (Params->HasField(TEXT("similarity")))
		{
			double Similarity = 0.0;
			if (!Params->TryGetNumberField(TEXT("similarity"), Similarity) || !(Similarity > 0.0) || Similarity > 1.0)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("similarity must be a number greater than 0 and at most 1"));
			}
			OutRequest.Similarity = Similarity;
		}
		return {};
	}

	/** One comparable unit: a graph, an event chain or a widget tree of one Blueprint. */
	struct FUnit
	{
		int32 BlueprintIndex = INDEX_NONE;
		FString Kind;
		FString Name;
		int32 NodeCount = 0;
		/** Hash of the normalised form; equal hashes mean exact duplicates. */
		FString Hash;
		/** Sorted node descriptors (widget descriptors for a widget tree): the multiset compared for similarity. */
		TArray<FString> Descriptors;
	};

	FString HashText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return LexToString(FIoHash::HashBuffer(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
	}

	/** Member reference as owner.name; self-context members of different Blueprints therefore compare equal. */
	FString MemberText(const FMemberReference& Reference)
	{
		FString Owner;
		if (Reference.IsSelfContext())
		{
			Owner = TEXT("self");
		}
		else if (Reference.IsLocalScope())
		{
			Owner = TEXT("local");
		}
		else if (const UClass* Parent = Reference.GetMemberParentClass())
		{
			Owner = Parent->GetPathName();
		}
		return FString::Printf(TEXT("%s.%s"), *Owner, *Reference.GetMemberName().ToString());
	}

	bool IsIncluded(const UEdGraphNode* Node)
	{
		return IsValid(Node) && !Node->IsA<UEdGraphNode_Comment>() && !Node->IsA<UK2Node_Knot>()
			&& Node->GetDesiredEnabledState() != ENodeEnabledState::Disabled;
	}

	bool IsExecPin(const UEdGraphPin* Pin)
	{
		return Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	/** Pins reached from Pin through links, looking through reroute knots. */
	void ResolveLinks(const UEdGraphPin* Pin, TArray<UEdGraphPin*>& OutPins, TSet<const UEdGraphNode*>& SeenKnots)
	{
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (!Linked)
			{
				continue;
			}
			if (const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Linked->GetOwningNode()))
			{
				if (!SeenKnots.Contains(Knot))
				{
					SeenKnots.Add(Knot);
					const UEdGraphPin* Next = Pin->Direction == EGPD_Output ? Knot->GetOutputPin() : Knot->GetInputPin();
					if (Next)
					{
						ResolveLinks(Next, OutPins, SeenKnots);
					}
				}
			}
			else
			{
				OutPins.Add(Linked);
			}
		}
	}

	/**
	 * Node descriptor: class, member reference and the values of unlinked, visible input pins that differ from the
	 * schema default (sorted, so pin order does not matter). GUIDs, names, positions and comments are never read.
	 */
	FString DescribeNode(const UEdGraphNode* Node)
	{
		FString Descriptor = Node->GetClass()->GetName();
		if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
		{
			Descriptor += TEXT("|event:") + CustomEvent->CustomFunctionName.ToString();
		}
		else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
		{
			Descriptor += TEXT("|event:") + MemberText(Event->EventReference);
		}
		else if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			Descriptor += TEXT("|fn:") + MemberText(Call->FunctionReference);
		}
		else if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
		{
			Descriptor += TEXT("|var:") + MemberText(Variable->VariableReference);
		}
		else if (const UK2Node_DynamicCast* Cast_ = Cast<UK2Node_DynamicCast>(Node))
		{
			Descriptor += TEXT("|cast:") + (Cast_->TargetType ? Cast_->TargetType->GetPathName() : FString());
		}
		else if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			const UEdGraph* MacroGraph = Macro->GetMacroGraph();
			Descriptor += TEXT("|macro:") + (MacroGraph ? MacroGraph->GetOutermost()->GetName() + TEXT(":") + MacroGraph->GetName() : FString());
		}
		else if (const UK2Node_StructOperation* StructOp = Cast<UK2Node_StructOperation>(Node))
		{
			Descriptor += TEXT("|struct:") + (StructOp->StructType ? StructOp->StructType->GetPathName() : FString());
		}

		const UEdGraphSchema* Schema = Node->GetSchema();
		TArray<FString> PinValues;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->bHidden || Pin->bOrphanedPin || Pin->LinkedTo.Num() > 0 || IsExecPin(Pin))
			{
				continue;
			}
			if (Schema && Schema->DoesDefaultValueMatchAutogenerated(*Pin))
			{
				continue;
			}
			const FString Value = Pin->GetDefaultAsString();
			if (!Value.IsEmpty())
			{
				PinValues.Add(FString::Printf(TEXT("%s=%s"), *Pin->PinName.ToString(), *Value));
			}
		}
		PinValues.Sort();
		for (const FString& PinValue : PinValues)
		{
			Descriptor += TEXT("|pin:") + PinValue;
		}
		return Descriptor;
	}

	struct FAdjacency
	{
		bool bOutgoing = false;
		FString OwnPin;
		FString OtherPin;
		int32 Other = INDEX_NONE;
	};

	/**
	 * Normalised form of a node set. Descriptors are refined iteratively (each round hashes a node's label with the
	 * labels of its linked neighbours and the pin names on both ends) until the partition of nodes into label
	 * classes stops growing. Rank of a node = position of the first node of its final label class in the sorted
	 * label order, so nodes that stay indistinguishable (identical subtrees, or the symmetric pairs a refinement
	 * cannot split) all receive the same rank and the emitted text never depends on how ties are ordered, node GUIDs,
	 * positions or the order of Graph->Nodes.
	 */
	void Normalise(const TArray<const UEdGraphNode*>& Nodes, FUnit& OutUnit)
	{
		const int32 Count = Nodes.Num();
		TMap<const UEdGraphNode*, int32> Index;
		Index.Reserve(Count);
		TArray<FString> Descriptors;
		Descriptors.Reserve(Count);
		for (int32 NodeIndex = 0; NodeIndex < Count; ++NodeIndex)
		{
			Index.Add(Nodes[NodeIndex], NodeIndex);
			Descriptors.Add(DescribeNode(Nodes[NodeIndex]));
		}

		struct FEdge
		{
			int32 From;
			FString FromPin;
			int32 To;
			FString ToPin;
		};
		TArray<FEdge> Edges;
		TArray<TArray<FAdjacency>> Adjacency;
		Adjacency.SetNum(Count);
		for (int32 NodeIndex = 0; NodeIndex < Count; ++NodeIndex)
		{
			for (const UEdGraphPin* Pin : Nodes[NodeIndex]->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output || Pin->bOrphanedPin)
				{
					continue;
				}
				TArray<UEdGraphPin*> Targets;
				TSet<const UEdGraphNode*> SeenKnots;
				ResolveLinks(Pin, Targets, SeenKnots);
				for (const UEdGraphPin* Target : Targets)
				{
					const int32* TargetIndex = Index.Find(Target->GetOwningNode());
					if (!TargetIndex)
					{
						continue;
					}
					const FString FromPin = Pin->PinName.ToString();
					const FString ToPin = Target->PinName.ToString();
					Edges.Add({NodeIndex, FromPin, *TargetIndex, ToPin});
					Adjacency[NodeIndex].Add({true, FromPin, ToPin, *TargetIndex});
					Adjacency[*TargetIndex].Add({false, ToPin, FromPin, NodeIndex});
				}
			}
		}

		TArray<FString> Labels;
		Labels.Reserve(Count);
		for (const FString& Descriptor : Descriptors)
		{
			Labels.Add(HashText(Descriptor));
		}
		const auto DistinctLabels = [](const TArray<FString>& Values)
		{
			TSet<FString> Distinct(Values);
			return Distinct.Num();
		};
		int32 Distinct = DistinctLabels(Labels);
		for (int32 Round = 0; Round < Count && Distinct < Count; ++Round)
		{
			TArray<FString> Next;
			Next.Reserve(Count);
			for (int32 NodeIndex = 0; NodeIndex < Count; ++NodeIndex)
			{
				TArray<FString> Neighbours;
				Neighbours.Reserve(Adjacency[NodeIndex].Num());
				for (const FAdjacency& Link : Adjacency[NodeIndex])
				{
					Neighbours.Add(FString::Printf(TEXT("%c|%s|%s|%s"), Link.bOutgoing ? TEXT('>') : TEXT('<'), *Link.OwnPin, *Link.OtherPin, *Labels[Link.Other]));
				}
				Neighbours.Sort();
				Next.Add(HashText(Labels[NodeIndex] + TEXT("\n") + FString::Join(Neighbours, TEXT("\n"))));
			}
			Labels = MoveTemp(Next);
			const int32 NextDistinct = DistinctLabels(Labels);
			if (NextDistinct == Distinct)
			{
				break;
			}
			Distinct = NextDistinct;
		}

		TArray<int32> Order;
		Order.Reserve(Count);
		for (int32 NodeIndex = 0; NodeIndex < Count; ++NodeIndex)
		{
			Order.Add(NodeIndex);
		}
		Algo::SortBy(Order, [&Labels](const int32 NodeIndex) { return Labels[NodeIndex]; });
		TArray<int32> Rank;
		Rank.SetNumUninitialized(Count);
		for (int32 Position = 0; Position < Count; ++Position)
		{
			const bool bSameAsPrevious = Position > 0 && Labels[Order[Position]] == Labels[Order[Position - 1]];
			Rank[Order[Position]] = bSameAsPrevious ? Rank[Order[Position - 1]] : Position;
		}

		TArray<FString> NodeLines;
		NodeLines.Reserve(Count);
		for (int32 Position = 0; Position < Count; ++Position)
		{
			NodeLines.Add(FString::Printf(TEXT("n|%d|%s"), Rank[Order[Position]], *Descriptors[Order[Position]]));
		}
		TArray<FString> EdgeLines;
		EdgeLines.Reserve(Edges.Num());
		for (const FEdge& Edge : Edges)
		{
			EdgeLines.Add(FString::Printf(TEXT("e|%d|%s|%d|%s"), Rank[Edge.From], *Edge.FromPin, Rank[Edge.To], *Edge.ToPin));
		}
		EdgeLines.Sort();

		OutUnit.NodeCount = Count;
		OutUnit.Hash = HashText(FString::Join(NodeLines, TEXT("\n")) + TEXT("\n") + FString::Join(EdgeLines, TEXT("\n")));
		OutUnit.Descriptors = MoveTemp(Descriptors);
		OutUnit.Descriptors.Sort();
	}

	/** Event node plus every node reachable from it over exec links, plus the pure producers feeding those nodes. */
	void CollectEventChain(const UK2Node_Event* Entry, TArray<const UEdGraphNode*>& OutNodes)
	{
		TSet<const UEdGraphNode*> Visited;
		TArray<const UEdGraphNode*> Pending;
		Visited.Add(Entry);
		Pending.Add(Entry);
		while (Pending.Num() > 0)
		{
			const UEdGraphNode* Node = Pending.Pop(EAllowShrinking::No);
			OutNodes.Add(Node);
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->bOrphanedPin)
				{
					continue;
				}
				const bool bForwardExec = Pin->Direction == EGPD_Output && IsExecPin(Pin);
				const bool bBackwardData = Pin->Direction == EGPD_Input && !IsExecPin(Pin);
				if (!bForwardExec && !bBackwardData)
				{
					continue;
				}
				TArray<UEdGraphPin*> Reached;
				TSet<const UEdGraphNode*> SeenKnots;
				ResolveLinks(Pin, Reached, SeenKnots);
				for (const UEdGraphPin* Other : Reached)
				{
					const UEdGraphNode* OtherNode = Other->GetOwningNode();
					if (!IsIncluded(OtherNode))
					{
						continue;
					}
					if (bBackwardData)
					{
						const UK2Node* K2Producer = Cast<UK2Node>(OtherNode);
						if (!K2Producer || !K2Producer->IsNodePure())
						{
							continue;
						}
					}
					if (!Visited.Contains(OtherNode))
					{
						Visited.Add(OtherNode);
						Pending.Add(OtherNode);
					}
				}
			}
		}
	}

	void AddGraphUnit(const UEdGraph* Graph, const TCHAR* Kind, const int32 BlueprintIndex, const FRequest& Request, TArray<FUnit>& OutUnits, int32& OutGraphs)
	{
		if (!Graph)
		{
			return;
		}
		TArray<const UEdGraphNode*> Nodes;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsIncluded(Node))
			{
				Nodes.Add(Node);
			}
		}
		++OutGraphs;
		if (Nodes.Num() < Request.MinNodes)
		{
			return;
		}
		FUnit Unit;
		Unit.BlueprintIndex = BlueprintIndex;
		Unit.Kind = Kind;
		Unit.Name = Graph->GetName();
		Normalise(Nodes, Unit);
		OutUnits.Add(MoveTemp(Unit));
	}

	void CollectBlueprintUnits(UBlueprint* Blueprint, const int32 BlueprintIndex, const FRequest& Request, TArray<FUnit>& OutUnits, int32& OutGraphs)
	{
		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			AddGraphUnit(Graph, UnitKind::Function, BlueprintIndex, Request, OutUnits, OutGraphs);
		}
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			for (const UEdGraph* Graph : Interface.Graphs)
			{
				AddGraphUnit(Graph, UnitKind::Function, BlueprintIndex, Request, OutUnits, OutGraphs);
			}
		}
		for (const UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			AddGraphUnit(Graph, UnitKind::Macro, BlueprintIndex, Request, OutUnits, OutGraphs);
		}
		for (const UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (!Graph)
			{
				continue;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
				if (!Event || !IsIncluded(Event))
				{
					continue;
				}
				++OutGraphs;
				TArray<const UEdGraphNode*> Nodes;
				CollectEventChain(Event, Nodes);
				if (Nodes.Num() < Request.MinNodes)
				{
					continue;
				}
				FUnit Unit;
				Unit.BlueprintIndex = BlueprintIndex;
				Unit.Kind = UnitKind::Event;
				Unit.Name = Event->GetFunctionName().ToString();
				Normalise(Nodes, Unit);
				OutUnits.Add(MoveTemp(Unit));
			}
		}

		// Widget trees are compared as class+name multisets of their widgets.
		const FObjectProperty* TreeProperty = CastField<FObjectProperty>(Blueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
		const UWidgetTree* Tree = TreeProperty ? Cast<UWidgetTree>(TreeProperty->GetObjectPropertyValue_InContainer(Blueprint)) : nullptr;
		if (Tree)
		{
			TArray<UWidget*> Widgets;
			Tree->GetAllWidgets(Widgets);
			++OutGraphs;
			if (Widgets.Num() >= Request.MinNodes)
			{
				FUnit Unit;
				Unit.BlueprintIndex = BlueprintIndex;
				Unit.Kind = UnitKind::WidgetTree;
				Unit.NodeCount = Widgets.Num();
				for (const UWidget* Widget : Widgets)
				{
					Unit.Descriptors.Add(FString::Printf(TEXT("%s|%s"), *Widget->GetClass()->GetName(), *Widget->GetName()));
				}
				Unit.Descriptors.Sort();
				Unit.Hash = HashText(FString::Join(Unit.Descriptors, TEXT("\n")));
				OutUnits.Add(MoveTemp(Unit));
			}
		}
	}

	/** Multiset Jaccard ratio of two sorted descriptor lists: shared / union, counting repeats. */
	double Jaccard(const TArray<FString>& Left, const TArray<FString>& Right)
	{
		int32 Shared = 0;
		int32 L = 0;
		int32 R = 0;
		while (L < Left.Num() && R < Right.Num())
		{
			const int32 Compare = Left[L].Compare(Right[R], ESearchCase::CaseSensitive);
			if (Compare == 0)
			{
				++Shared;
				++L;
				++R;
			}
			else if (Compare < 0)
			{
				++L;
			}
			else
			{
				++R;
			}
		}
		const int32 Union = Left.Num() + Right.Num() - Shared;
		return Union > 0 ? static_cast<double>(Shared) / Union : 0.0;
	}

	struct FGroup
	{
		FString Kind;
		FString Name;
		FString Hash;
		int32 NodeCount = 0;
		/** Weakest linking pair of a similar group; 1.0 for exact groups. */
		double Similarity = 1.0;
		TArray<int32> Members;
	};

	int32 FindRoot(TArray<int32>& Parents, int32 Item)
	{
		while (Parents[Item] != Item)
		{
			Parents[Item] = Parents[Parents[Item]];
			Item = Parents[Item];
		}
		return Item;
	}

	TSharedRef<FJsonObject> MemberJson(const FUnit& Unit, const TArray<FString>& BlueprintPaths)
	{
		TSharedRef<FJsonObject> Member = MakeShared<FJsonObject>();
		Member->SetStringField(TEXT("asset_path"), BlueprintPaths[Unit.BlueprintIndex]);
		Member->SetStringField(TEXT("graph_name"), Unit.Kind == UnitKind::WidgetTree ? FString(TEXT("WidgetTree")) : Unit.Name);
		Member->SetNumberField(TEXT("node_count"), Unit.NodeCount);
		return Member;
	}

	TArray<TSharedPtr<FJsonValue>> GroupsJson(
		TArray<FGroup>& Groups, const TArray<FUnit>& Units, const TArray<FString>& BlueprintPaths, const bool bExact)
	{
		for (FGroup& Group : Groups)
		{
			Algo::Sort(Group.Members, [&](const int32 Left, const int32 Right)
			{
				const FUnit& L = Units[Left];
				const FUnit& R = Units[Right];
				if (L.BlueprintIndex != R.BlueprintIndex)
				{
					return BlueprintPaths[L.BlueprintIndex] < BlueprintPaths[R.BlueprintIndex];
				}
				return L.Name < R.Name;
			});
		}
		Algo::Sort(Groups, [](const FGroup& Left, const FGroup& Right)
		{
			if (Left.Members.Num() != Right.Members.Num())
			{
				return Left.Members.Num() > Right.Members.Num();
			}
			if (Left.NodeCount != Right.NodeCount)
			{
				return Left.NodeCount > Right.NodeCount;
			}
			if (Left.Kind != Right.Kind)
			{
				return Left.Kind < Right.Kind;
			}
			return Left.Hash != Right.Hash ? Left.Hash < Right.Hash : Left.Name < Right.Name;
		});

		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FGroup& Group : Groups)
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("kind"), Group.Kind);
			if (bExact)
			{
				Json->SetStringField(TEXT("hash"), Group.Hash);
				Json->SetNumberField(TEXT("node_count"), Group.NodeCount);
			}
			else
			{
				Json->SetStringField(TEXT("name"), Group.Name);
				Json->SetNumberField(TEXT("similarity"), Group.Similarity);
			}
			TArray<TSharedPtr<FJsonValue>> Members;
			for (const int32 Member : Group.Members)
			{
				Members.Add(MakeShared<FJsonValueObject>(MemberJson(Units[Member], BlueprintPaths)));
			}
			Json->SetArrayField(TEXT("members"), Members);
			Values.Add(MakeShared<FJsonValueObject>(Json));
		}
		return Values;
	}
}

FCortexCommandResult FCortexReflectDuplicateGraphOps::FindDuplicateGraphs(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexReflectDuplicateGraphPrivate;

	FRequest Request;
	if (TOptional<FCortexCommandResult> Error = ParseRequest(Params, Request))
	{
		return Error.GetValue();
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	for (const FString& Prefix : Request.Prefixes)
	{
		Registry.ScanPathsSynchronous({Prefix}, true);
	}
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	for (const FString& Prefix : Request.Prefixes)
	{
		Filter.PackagePaths.Add(FName(*Prefix));
	}
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	const FTopLevelAssetPath LevelScriptClassPath = ULevelScriptBlueprint::StaticClass()->GetClassPathName();
	Assets.RemoveAll([&LevelScriptClassPath](const FAssetData& Asset) { return Asset.AssetClassPath == LevelScriptClassPath; });
	Algo::SortBy(Assets, [](const FAssetData& Asset) { return Asset.GetObjectPathString(); });

	// Each Blueprint is loaded once; its units are extracted immediately and the loaded objects are released by one
	// collection after the run.
	TArray<FString> BlueprintPaths;
	TArray<FUnit> Units;
	TArray<FString> LoadFailures;
	int32 GraphsScanned = 0;
	for (const FAssetData& Asset : Assets)
	{
		const FString AssetPath = Asset.GetObjectPathString();
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint)
		{
			LoadFailures.Add(AssetPath);
			continue;
		}
		const int32 BlueprintIndex = BlueprintPaths.Add(AssetPath);
		CollectBlueprintUnits(Blueprint, BlueprintIndex, Request, Units, GraphsScanned);
	}
	if (BlueprintPaths.Num() > 0)
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}

	// Exact groups: identical hash (per kind) across at least two Blueprints.
	TMap<FString, TArray<int32>> ByHash;
	for (int32 UnitIndex = 0; UnitIndex < Units.Num(); ++UnitIndex)
	{
		ByHash.FindOrAdd(Units[UnitIndex].Kind + TEXT("|") + Units[UnitIndex].Hash).Add(UnitIndex);
	}
	TArray<FGroup> ExactGroups;
	for (const TPair<FString, TArray<int32>>& Entry : ByHash)
	{
		TSet<int32> Blueprints;
		for (const int32 UnitIndex : Entry.Value)
		{
			Blueprints.Add(Units[UnitIndex].BlueprintIndex);
		}
		if (Blueprints.Num() < 2)
		{
			continue;
		}
		const FUnit& First = Units[Entry.Value[0]];
		FGroup Group;
		Group.Kind = First.Kind;
		Group.Hash = First.Hash;
		Group.NodeCount = First.NodeCount;
		Group.Members = Entry.Value;
		ExactGroups.Add(MoveTemp(Group));
	}

	// Similar groups: same kind and name (any widget tree against any other) in different Blueprints, differing
	// hash, Jaccard ratio of the descriptor multisets at or above the threshold. A group is a connected component
	// of such pairs and reports its weakest linking pair.
	TMap<FString, TArray<int32>> ByName;
	for (int32 UnitIndex = 0; UnitIndex < Units.Num(); ++UnitIndex)
	{
		const FUnit& Unit = Units[UnitIndex];
		ByName.FindOrAdd(Unit.Kind + TEXT("|") + (Unit.Kind == UnitKind::WidgetTree ? FString() : Unit.Name)).Add(UnitIndex);
	}
	TArray<FGroup> SimilarGroups;
	TSet<TPair<int32, int32>> WidgetPairs;
	for (const TPair<FString, TArray<int32>>& Bucket : ByName)
	{
		const TArray<int32>& Members = Bucket.Value;
		if (Members.Num() < 2)
		{
			continue;
		}
		struct FLink
		{
			int32 Left;
			int32 Right;
			double Similarity;
		};
		TArray<FLink> Links;
		TArray<int32> Parents;
		Parents.SetNumUninitialized(Members.Num());
		for (int32 Slot = 0; Slot < Members.Num(); ++Slot)
		{
			Parents[Slot] = Slot;
		}
		for (int32 Left = 0; Left < Members.Num(); ++Left)
		{
			for (int32 Right = Left + 1; Right < Members.Num(); ++Right)
			{
				const FUnit& L = Units[Members[Left]];
				const FUnit& R = Units[Members[Right]];
				if (L.BlueprintIndex == R.BlueprintIndex)
				{
					continue;
				}
				const bool bWidget = L.Kind == UnitKind::WidgetTree;
				if (L.Hash == R.Hash)
				{
					if (bWidget)
					{
						WidgetPairs.Add({FMath::Min(L.BlueprintIndex, R.BlueprintIndex), FMath::Max(L.BlueprintIndex, R.BlueprintIndex)});
					}
					continue;
				}
				const double Similarity = Jaccard(L.Descriptors, R.Descriptors);
				if (Similarity < Request.Similarity)
				{
					continue;
				}
				Links.Add({Left, Right, Similarity});
				Parents[FindRoot(Parents, Left)] = FindRoot(Parents, Right);
				if (bWidget)
				{
					WidgetPairs.Add({FMath::Min(L.BlueprintIndex, R.BlueprintIndex), FMath::Max(L.BlueprintIndex, R.BlueprintIndex)});
				}
			}
		}
		TMap<int32, FGroup> Components;
		for (const FLink& Link : Links)
		{
			const int32 Root = FindRoot(Parents, Link.Left);
			FGroup& Group = Components.FindOrAdd(Root);
			if (Group.Members.Num() == 0)
			{
				const FUnit& First = Units[Members[Link.Left]];
				Group.Kind = First.Kind;
				Group.Name = First.Kind == UnitKind::WidgetTree ? FString() : First.Name;
				Group.Similarity = 1.0;
			}
			Group.Similarity = FMath::Min(Group.Similarity, Link.Similarity);
		}
		for (int32 Slot = 0; Slot < Members.Num(); ++Slot)
		{
			if (FGroup* Group = Components.Find(FindRoot(Parents, Slot)))
			{
				Group->Members.Add(Members[Slot]);
				Group->NodeCount = FMath::Max(Group->NodeCount, Units[Members[Slot]].NodeCount);
			}
		}
		for (TPair<int32, FGroup>& Component : Components)
		{
			SimilarGroups.Add(MoveTemp(Component.Value));
		}
	}

	// Blueprint pairs: how many exact/similar graph groups (widget trees aside) two Blueprints share, plus the
	// similarity of their widget trees. Pairs qualify through a shared graph group or a widget-tree match.
	struct FPairStats
	{
		int32 SharedGraphs = 0;
	};
	TMap<TPair<int32, int32>, FPairStats> Pairs;
	const auto CountGroups = [&](const TArray<FGroup>& Groups)
	{
		for (const FGroup& Group : Groups)
		{
			if (Group.Kind == UnitKind::WidgetTree)
			{
				continue;
			}
			TArray<int32> Blueprints;
			for (const int32 Member : Group.Members)
			{
				Blueprints.AddUnique(Units[Member].BlueprintIndex);
			}
			Blueprints.Sort();
			for (int32 Left = 0; Left < Blueprints.Num(); ++Left)
			{
				for (int32 Right = Left + 1; Right < Blueprints.Num(); ++Right)
				{
					++Pairs.FindOrAdd({Blueprints[Left], Blueprints[Right]}).SharedGraphs;
				}
			}
		}
	};
	CountGroups(ExactGroups);
	CountGroups(SimilarGroups);
	for (const TPair<int32, int32>& WidgetPair : WidgetPairs)
	{
		Pairs.FindOrAdd(WidgetPair);
	}
	TMap<int32, int32> WidgetUnitOfBlueprint;
	for (int32 UnitIndex = 0; UnitIndex < Units.Num(); ++UnitIndex)
	{
		if (Units[UnitIndex].Kind == UnitKind::WidgetTree)
		{
			WidgetUnitOfBlueprint.Add(Units[UnitIndex].BlueprintIndex, UnitIndex);
		}
	}
	struct FPairRow
	{
		FString A;
		FString B;
		int32 SharedGraphs;
		double WidgetSimilarity;
	};
	TArray<FPairRow> PairRows;
	for (const TPair<TPair<int32, int32>, FPairStats>& Pair : Pairs)
	{
		double WidgetSimilarity = 0.0;
		const int32* LeftWidget = WidgetUnitOfBlueprint.Find(Pair.Key.Key);
		const int32* RightWidget = WidgetUnitOfBlueprint.Find(Pair.Key.Value);
		if (LeftWidget && RightWidget)
		{
			WidgetSimilarity = Jaccard(Units[*LeftWidget].Descriptors, Units[*RightWidget].Descriptors);
		}
		PairRows.Add({BlueprintPaths[Pair.Key.Key], BlueprintPaths[Pair.Key.Value], Pair.Value.SharedGraphs, WidgetSimilarity});
	}
	Algo::Sort(PairRows, [](const FPairRow& Left, const FPairRow& Right)
	{
		if (Left.SharedGraphs != Right.SharedGraphs)
		{
			return Left.SharedGraphs > Right.SharedGraphs;
		}
		if (Left.WidgetSimilarity != Right.WidgetSimilarity)
		{
			return Left.WidgetSimilarity > Right.WidgetSimilarity;
		}
		return Left.A != Right.A ? Left.A < Right.A : Left.B < Right.B;
	});

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("blueprints_scanned"), BlueprintPaths.Num());
	Result->SetNumberField(TEXT("graphs_scanned"), GraphsScanned);
	Result->SetNumberField(TEXT("min_nodes"), Request.MinNodes);
	Result->SetNumberField(TEXT("similarity_threshold"), Request.Similarity);
	TArray<TSharedPtr<FJsonValue>> Failures;
	for (const FString& Failure : LoadFailures)
	{
		Failures.Add(MakeShared<FJsonValueString>(Failure));
	}
	Result->SetArrayField(TEXT("load_failures"), Failures);
	Result->SetArrayField(TEXT("exact_groups"), GroupsJson(ExactGroups, Units, BlueprintPaths, true));
	Result->SetArrayField(TEXT("similar_groups"), GroupsJson(SimilarGroups, Units, BlueprintPaths, false));
	TArray<TSharedPtr<FJsonValue>> PairValues;
	for (const FPairRow& Row : PairRows)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("a"), Row.A);
		Json->SetStringField(TEXT("b"), Row.B);
		Json->SetNumberField(TEXT("shared_graphs"), Row.SharedGraphs);
		Json->SetNumberField(TEXT("widget_tree_similarity"), Row.WidgetSimilarity);
		PairValues.Add(MakeShared<FJsonValueObject>(Json));
	}
	Result->SetArrayField(TEXT("blueprint_pairs"), PairValues);
	UE_LOG(LogCortexReflect, Log, TEXT("find_duplicate_graphs: %d Blueprints, %d graphs, %d units, %d exact, %d similar, %d pairs"),
		BlueprintPaths.Num(), GraphsScanned, Units.Num(), ExactGroups.Num(), SimilarGroups.Num(), PairRows.Num());
	return FCortexCommandRouter::Success(Result);
}
