#include "Operations/CortexReflectDuplicateGraphOps.h"

#include "Algo/Sort.h"
#include "Operations/CortexReflectAssetEnumeration.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
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
#include "Engine/MemberReference.h"
#include "IO/IoHash.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_StructOperation.h"
#include "K2Node_Variable.h"
#include "Modules/ModuleManager.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"
#include "Internationalization/Regex.h"
#include "Components/PanelWidget.h"

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
		/** Optional report file; with it the response is a compact summary. */
		bool bHasOutPath = false;
		FCortexResolvedFilePath OutPath;
	};

	/** Validates every parameter before any asset is touched. Returns an error result or an empty optional. */
	TOptional<FCortexCommandResult> ParseRequest(const TSharedPtr<FJsonObject>& Params, FRequest& OutRequest)
	{
		if (!Params.IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
		}

		FCortexCommandResult PrefixError;
		if (!CortexReflectAssetEnumeration::ParsePathPrefixes(Params, OutRequest.Prefixes, PrefixError))
		{
			return PrefixError;
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

		FCortexCommandResult OutPathError;
		if (!CortexReflectAssetEnumeration::ParseOptionalOutPath(Params, OutRequest.bHasOutPath, OutRequest.OutPath, OutPathError))
		{
			return OutPathError;
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

	/** Lower-case text without spaces and underscores: Blueprint class display names are prettified. */
	FString StripSeparators(const FString& Text)
	{
		return Text.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
	}

	/** Name of the nearest native ancestor of a Blueprint-generated class (the class itself when native). */
	FString NativeAncestorName(const UClass* Class)
	{
		while (Class && Class->ClassGeneratedBy)
		{
			Class = Class->GetSuperClass();
		}
		return Class ? Class->GetName() : FString(TEXT("BlueprintClass"));
	}

	/**
	 * Loose form of a text holding object paths: every `<path>_C` Blueprint-generated class path becomes the name
	 * of its nearest native ancestor, so parallel sibling Blueprint classes compare equal.
	 */
	FString LoosenClasses(const FString& Value)
	{
		static const FRegexPattern Pattern(TEXT("/[A-Za-z0-9_/.\\-]+_C(?![A-Za-z0-9_])"));
		FRegexMatcher Matcher(Pattern, Value);
		FString Result;
		int32 Last = 0;
		while (Matcher.FindNext())
		{
			Result += Value.Mid(Last, Matcher.GetMatchBeginning() - Last);
			const UClass* Class = FindObject<UClass>(nullptr, *Matcher.GetCaptureGroup(0));
			Result += Class ? NativeAncestorName(Class) : FString(TEXT("BlueprintClass"));
			Last = Matcher.GetMatchEnding();
		}
		return Result + Value.Mid(Last);
	}
	/**
	 * The Blueprint that owns the graphs being normalised. Every textual reference to the Blueprint itself (its
	 * generated class, skeleton class, `_C` class path, object path) becomes `self`, so two copies of a graph
	 * that cast to, or pass, "their own class" compare equal. Cast output pins named after the Blueprint
	 * (`As<Name>`) become `Asself`.
	 */
	struct FSelfContext
	{
		TArray<FString> SelfTexts;
		FString CastPinName;

		explicit FSelfContext(const UBlueprint* Blueprint)
		{
			const FString PackageName = Blueprint->GetOutermost()->GetName();
			const FString Name = Blueprint->GetName();
			SelfTexts.Add(Blueprint->GetPathName());
			SelfTexts.Add(FString::Printf(TEXT("%s.%s_C"), *PackageName, *Name));
			SelfTexts.Add(FString::Printf(TEXT("%s.SKEL_%s_C"), *PackageName, *Name));
			SelfTexts.Add(GetPathNameSafe(Blueprint->GeneratedClass));
			SelfTexts.Add(GetPathNameSafe(Blueprint->SkeletonGeneratedClass));
			SelfTexts.RemoveAll([](const FString& Text) { return Text.IsEmpty(); });
			// Longest first so `<path>_C` is replaced before its `<path>` prefix.
			SelfTexts.Sort([](const FString& Left, const FString& Right) { return Left.Len() > Right.Len(); });
			CastPinName = StripSeparators(Name);
		}

		bool bLoose = false;

		FString Text(FString Value) const
		{
			for (const FString& SelfText : SelfTexts)
			{
				Value.ReplaceInline(*SelfText, TEXT("self"), ESearchCase::CaseSensitive);
			}
			return bLoose ? LoosenClasses(Value) : Value;
		}

		FString PinName(const FName Name) const
		{
			const FString Raw = Name.ToString();
			return (Raw.StartsWith(TEXT("As")) && StripSeparators(Raw.Mid(2)) == CastPinName) ? FString(TEXT("Asself")) : Raw;
		}
	};

	/** Category, subcategory, subcategory object, container kind and reference/const flags of a pin type. */
	FString TypeText(const FEdGraphPinType& Type, const FSelfContext& Self)
	{
		FString Text = FString::Printf(TEXT("%s/%s/%s/c%d%s%s"), *Type.PinCategory.ToString(), *Type.PinSubCategory.ToString(),
			*Self.Text(GetPathNameSafe(Type.PinSubCategoryObject.Get())), static_cast<int32>(Type.ContainerType),
			Type.bIsReference ? TEXT("&") : TEXT(""), Type.bIsConst ? TEXT("k") : TEXT(""));
		if (Type.ContainerType == EPinContainerType::Map)
		{
			Text += FString::Printf(TEXT("->%s/%s/%s"), *Type.PinValueType.TerminalCategory.ToString(),
				*Type.PinValueType.TerminalSubCategory.ToString(), *Self.Text(GetPathNameSafe(Type.PinValueType.TerminalSubCategoryObject.Get())));
		}
		return Text;
	}

	/** Pin name plus its type: the identity of one end of a link. */
	FString PinKey(const UEdGraphPin* Pin, const FSelfContext& Self)
	{
		return Self.PinName(Pin->PinName) + TEXT(":") + TypeText(Pin->PinType, Self);
	}

	/** Member reference as owner.name; self-context members of different Blueprints therefore compare equal. */
	FString MemberText(const FMemberReference& Reference, const FSelfContext& Self)
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
			Owner = (Self.bLoose && Parent->ClassGeneratedBy) ? FString(TEXT("bp")) : Self.Text(Parent->GetPathName());
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
	 * Node descriptor, built only from content (never GUIDs, names, positions or comments):
	 *  - class and member reference (function, variable, event, delegate, cast target, macro graph, struct);
	 *  - for nodes with editable pins (function entry/result, custom events, tunnels) every pin with direction,
	 *    name and type, so an unused parameter still separates two graphs;
	 *  - the node's reflected scalar properties (numeric, bool, name, string, enum, object/class references),
	 *    skipping transient/deprecated ones and everything declared on UEdGraphNode (cosmetics). This gives
	 *    node classes without a dedicated rule identity: literal object, enum of enum literal/switch/select,
	 *    delegate function name, timeline name. Struct, array and map properties are not read: they carry GUIDs
	 *    (FMemberReference, timeline GUID) or editor-only layout. A collapsed-graph node is described by its bound
	 *    graph's path only; the sub-graph's own nodes are not expanded into the parent;
	 *  - non-default values of unlinked, visible input pins, sorted by pin name.
	 */
	FString DescribeNode(const UEdGraphNode* Node, const FSelfContext& Self)
	{
		FString Descriptor = Node->GetClass()->GetName();
		if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
		{
			Descriptor += TEXT("|event:") + CustomEvent->CustomFunctionName.ToString();
		}
		else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
		{
			Descriptor += TEXT("|event:") + MemberText(Event->EventReference, Self);
		}
		else if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			Descriptor += TEXT("|fn:") + MemberText(Call->FunctionReference, Self);
		}
		else if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
		{
			Descriptor += TEXT("|var:") + MemberText(Variable->VariableReference, Self);
		}
		else if (const UK2Node_BaseMCDelegate* Delegate = Cast<UK2Node_BaseMCDelegate>(Node))
		{
			Descriptor += TEXT("|delegate:") + MemberText(Delegate->DelegateReference, Self);
		}
		else if (const UK2Node_DynamicCast* Cast_ = Cast<UK2Node_DynamicCast>(Node))
		{
			Descriptor += TEXT("|cast:") + Self.Text(Cast_->TargetType ? Cast_->TargetType->GetPathName() : FString());
		}
		else if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			const UEdGraph* MacroGraph = Macro->GetMacroGraph();
			Descriptor += TEXT("|macro:") + Self.Text(MacroGraph ? MacroGraph->GetOutermost()->GetName() + TEXT(":") + MacroGraph->GetName() : FString());
		}
		else if (const UK2Node_StructOperation* StructOp = Cast<UK2Node_StructOperation>(Node))
		{
			Descriptor += TEXT("|struct:") + Self.Text(StructOp->StructType ? StructOp->StructType->GetPathName() : FString());
		}
		if (Cast<UK2Node_EditablePinBase>(Node))
		{
			TArray<FString> Signature;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && !Pin->bOrphanedPin)
				{
					Signature.Add(FString::Printf(TEXT("%c%s"), Pin->Direction == EGPD_Input ? TEXT('i') : TEXT('o'), *PinKey(Pin, Self)));
				}
			}
			Signature.Sort();
			for (const FString& Entry : Signature)
			{
				Descriptor += TEXT("|sig:") + Entry;
			}
		}

		TArray<FString> Properties;
		for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			const UClass* Owner = Property->GetOwnerClass();
			if ((Owner && UEdGraphNode::StaticClass()->IsChildOf(Owner))
				|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated)
				|| !(CastField<FNumericProperty>(Property) || CastField<FBoolProperty>(Property) || CastField<FNameProperty>(Property)
					|| CastField<FStrProperty>(Property) || CastField<FEnumProperty>(Property) || CastField<FObjectPropertyBase>(Property)))
			{
				continue;
			}
			FString Value;
			Property->ExportText_InContainer(0, Value, Node, nullptr, nullptr, PPF_None);
			Properties.Add(FString::Printf(TEXT("%s=%s"), *Property->GetName(), *Self.Text(Value)));
		}
		Properties.Sort();
		for (const FString& Property : Properties)
		{
			Descriptor += TEXT("|prop:") + Property;
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
				PinValues.Add(FString::Printf(TEXT("%s=%s"), *Self.PinName(Pin->PinName), *Self.Text(Value)));
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
	 * labels of its linked neighbours and the typed pin keys on both ends) until the partition of nodes into label
	 * classes stops growing (1-dimensional Weisfeiler-Leman). Rank of a node = position of the first node of its
	 * final label class in the sorted label order, so nodes that stay indistinguishable all receive the same rank
	 * and the emitted text never depends on tie order, node GUIDs, positions or the order of Graph->Nodes.
	 * Limitation: refinement is not a full isomorphism test; two non-isomorphic graphs it cannot distinguish
	 * (rare regular structures) hash equal. Typical Blueprint graphs are separated.
	 */
	void Normalise(const TArray<const UEdGraphNode*>& Nodes, const FSelfContext& Self, FUnit& OutUnit)
	{
		const int32 Count = Nodes.Num();
		TMap<const UEdGraphNode*, int32> Index;
		Index.Reserve(Count);
		TArray<FString> Descriptors;
		Descriptors.Reserve(Count);
		for (int32 NodeIndex = 0; NodeIndex < Count; ++NodeIndex)
		{
			Index.Add(Nodes[NodeIndex], NodeIndex);
			Descriptors.Add(DescribeNode(Nodes[NodeIndex], Self));
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
					const FString FromPin = PinKey(Pin, Self);
					const FString ToPin = PinKey(Target, Self);
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
		// Similarity compares the loose descriptors; the hash above stays strict.
		FSelfContext LooseSelf = Self;
		LooseSelf.bLoose = true;
		OutUnit.Descriptors.Reset(Count);
		for (const UEdGraphNode* Node : Nodes)
		{
			OutUnit.Descriptors.Add(DescribeNode(Node, LooseSelf));
		}
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

	void AddGraphUnit(const UEdGraph* Graph, const TCHAR* Kind, const int32 BlueprintIndex, const FRequest& Request, const FSelfContext& Self, TArray<FUnit>& OutUnits, int32& OutGraphs)
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
		Normalise(Nodes, Self, Unit);
		OutUnits.Add(MoveTemp(Unit));
	}

	void CollectBlueprintUnits(UBlueprint* Blueprint, const int32 BlueprintIndex, const FRequest& Request, TArray<FUnit>& OutUnits, int32& OutGraphs)
	{
		const FSelfContext Self(Blueprint);
		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			AddGraphUnit(Graph, UnitKind::Function, BlueprintIndex, Request, Self, OutUnits, OutGraphs);
		}
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			for (const UEdGraph* Graph : Interface.Graphs)
			{
				AddGraphUnit(Graph, UnitKind::Function, BlueprintIndex, Request, Self, OutUnits, OutGraphs);
			}
		}
		for (const UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			AddGraphUnit(Graph, UnitKind::Macro, BlueprintIndex, Request, Self, OutUnits, OutGraphs);
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
				Normalise(Nodes, Self, Unit);
				OutUnits.Add(MoveTemp(Unit));
			}
		}

		// Widget trees are compared as class+name multisets of their widgets. The strict hash keeps the full class path
		// (child widget classes of different packages can share a short name); the loose descriptors use the native ancestor.
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
				TArray<FString> Strict;
				for (const UWidget* Widget : Widgets)
				{
					const UPanelWidget* Parent = Widget->GetParent();
					Unit.Descriptors.Add(FString::Printf(TEXT("%s|%s|%s"), *Widget->GetName(),
						*NativeAncestorName(Widget->GetClass()), Parent ? *Parent->GetName() : TEXT("")));
					Strict.Add(FString::Printf(TEXT("%s|%s"), *Self.Text(Widget->GetClass()->GetPathName()), *Widget->GetName()));
				}
				Unit.Descriptors.Sort();
				Strict.Sort();
				Unit.Hash = HashText(FString::Join(Strict, TEXT("\n")));
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
		/** Similar groups: true minimum and maximum Jaccard over all member pairs. Exact groups: 1.0. */
		double MinSimilarity = 1.0;
		double MaxSimilarity = 1.0;
		/** Similar groups: the unit every other member was linked to. */
		int32 Representative = INDEX_NONE;
		TArray<int32> Members;
	};

	/** Stable unit order: Blueprint path, graph name, kind. */
	bool UnitLess(const TArray<FUnit>& Units, const TArray<FString>& BlueprintPaths, const int32 Left, const int32 Right)
	{
		const FUnit& L = Units[Left];
		const FUnit& R = Units[Right];
		if (L.BlueprintIndex != R.BlueprintIndex)
		{
			return BlueprintPaths[L.BlueprintIndex] < BlueprintPaths[R.BlueprintIndex];
		}
		return L.Name != R.Name ? L.Name < R.Name : L.Kind < R.Kind;
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
			Algo::Sort(Group.Members, [&](const int32 Left, const int32 Right) { return UnitLess(Units, BlueprintPaths, Left, Right); });
		}
		Algo::Sort(Groups, [&](const FGroup& Left, const FGroup& Right)
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
			if (Left.Hash != Right.Hash)
			{
				return Left.Hash < Right.Hash;
			}
			if (Left.Name != Right.Name)
			{
				return Left.Name < Right.Name;
			}
			return UnitLess(Units, BlueprintPaths, Left.Members[0], Right.Members[0]);
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
				Json->SetStringField(TEXT("representative"), BlueprintPaths[Units[Group.Representative].BlueprintIndex]);
				Json->SetNumberField(TEXT("min_similarity"), Group.MinSimilarity);
				Json->SetNumberField(TEXT("max_similarity"), Group.MaxSimilarity);
				Json->SetBoolField(TEXT("loose"), true);
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

	// Blueprints are enumerated (and prefixes checked for matches) before any is loaded.
	const CortexReflectAssetEnumeration::FEnumeration Enumeration =
		CortexReflectAssetEnumeration::Enumerate(Request.Prefixes, {UBlueprint::StaticClass()->GetClassPathName()});
	const TArray<FAssetData>& Assets = Enumeration.Assets;

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

	// Exact groups: identical hash (per kind) across at least two Blueprints. Every cross-Blueprint unit pair of a
	// group is recorded once in SharedUnitPairs, the single source of the Blueprint pair statistics.
	TSet<TPair<int32, int32>> SharedUnitPairs;
	const auto AddUnitPair = [&Units, &SharedUnitPairs](const int32 Left, const int32 Right)
	{
		if (Units[Left].BlueprintIndex != Units[Right].BlueprintIndex)
		{
			SharedUnitPairs.Add({FMath::Min(Left, Right), FMath::Max(Left, Right)});
		}
	};
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
		for (int32 Left = 0; Left < Entry.Value.Num(); ++Left)
		{
			for (int32 Right = Left + 1; Right < Entry.Value.Num(); ++Right)
			{
				AddUnitPair(Entry.Value[Left], Entry.Value[Right]);
			}
		}
		ExactGroups.Add(MoveTemp(Group));
	}

	// Similar groups. Candidates are same kind and name (any widget tree against any other). Units with identical
	// hashes collapse to one class represented by its first unit (path, name), so an exact copy never appears again
	// among the similar members. Classes are linked when their descriptor multisets have a Jaccard ratio at or
	// above the threshold, then grouped as star clusters: the class with the highest total similarity to still
	// unassigned classes (ties: first representative) becomes the representative and takes every unassigned class
	// linked to it. A cluster reports the true minimum and maximum Jaccard over all its member pairs, which can be
	// below the threshold for members that are only linked through the representative.
	// Pairwise work: Jaccard <= min(|A|,|B|) / max(|A|,|B|), so classes sorted by descriptor count stop comparing
	// as soon as the size ratio drops under the threshold; worst case O(n^2) per same-name bucket, typically far less.
	TMap<FString, TArray<int32>> ByName;
	for (int32 UnitIndex = 0; UnitIndex < Units.Num(); ++UnitIndex)
	{
		const FUnit& Unit = Units[UnitIndex];
		ByName.FindOrAdd(Unit.Kind + TEXT("|") + (Unit.Kind == UnitKind::WidgetTree ? FString() : Unit.Name)).Add(UnitIndex);
	}
	TArray<FGroup> SimilarGroups;
	for (const TPair<FString, TArray<int32>>& Bucket : ByName)
	{
		TMap<FString, TArray<int32>> ClassByHash;
		for (const int32 UnitIndex : Bucket.Value)
		{
			ClassByHash.FindOrAdd(Units[UnitIndex].Hash).Add(UnitIndex);
		}
		if (ClassByHash.Num() < 2)
		{
			continue;
		}
		TArray<TArray<int32>> Classes;
		for (TPair<FString, TArray<int32>>& Class : ClassByHash)
		{
			Algo::Sort(Class.Value, [&](const int32 Left, const int32 Right) { return UnitLess(Units, BlueprintPaths, Left, Right); });
			Classes.Add(MoveTemp(Class.Value));
		}
		Algo::Sort(Classes, [&](const TArray<int32>& Left, const TArray<int32>& Right) { return UnitLess(Units, BlueprintPaths, Left[0], Right[0]); });
		const int32 ClassCount = Classes.Num();
		const auto Descriptors = [&](const int32 ClassIndex) -> const TArray<FString>& { return Units[Classes[ClassIndex][0]].Descriptors; };
		const auto HasCrossBlueprint = [&](const int32 Left, const int32 Right)
		{
			for (const int32 L : Classes[Left])
			{
				for (const int32 R : Classes[Right])
				{
					if (Units[L].BlueprintIndex != Units[R].BlueprintIndex)
					{
						return true;
					}
				}
			}
			return false;
		};

		TArray<int32> BySize;
		for (int32 ClassIndex = 0; ClassIndex < ClassCount; ++ClassIndex)
		{
			BySize.Add(ClassIndex);
		}
		Algo::Sort(BySize, [&](const int32 Left, const int32 Right)
		{
			const int32 LeftSize = Descriptors(Left).Num();
			const int32 RightSize = Descriptors(Right).Num();
			return LeftSize != RightSize ? LeftSize < RightSize : Left < Right;
		});
		TArray<TArray<TPair<int32, double>>> Links;
		Links.SetNum(ClassCount);
		for (int32 Small = 0; Small < ClassCount; ++Small)
		{
			const int32 SmallClass = BySize[Small];
			const double SmallSize = Descriptors(SmallClass).Num();
			for (int32 Large = Small + 1; Large < ClassCount; ++Large)
			{
				const int32 LargeClass = BySize[Large];
				if (SmallSize / Descriptors(LargeClass).Num() < Request.Similarity)
				{
					break;
				}
				if (!HasCrossBlueprint(SmallClass, LargeClass))
				{
					continue;
				}
				const double Similarity = Jaccard(Descriptors(SmallClass), Descriptors(LargeClass));
				if (Similarity >= Request.Similarity)
				{
					Links[SmallClass].Add({LargeClass, Similarity});
					Links[LargeClass].Add({SmallClass, Similarity});
				}
			}
		}

		TArray<bool> Assigned;
		Assigned.Init(false, ClassCount);
		while (true)
		{
			int32 Representative = INDEX_NONE;
			double BestTotal = 0.0;
			for (int32 ClassIndex = 0; ClassIndex < ClassCount; ++ClassIndex)
			{
				if (Assigned[ClassIndex])
				{
					continue;
				}
				double Total = 0.0;
				for (const TPair<int32, double>& Link : Links[ClassIndex])
				{
					Total += Assigned[Link.Key] ? 0.0 : Link.Value;
				}
				if (Total > BestTotal + 1e-12)
				{
					BestTotal = Total;
					Representative = ClassIndex;
				}
			}
			if (Representative == INDEX_NONE)
			{
				break;
			}
			TArray<int32> Cluster;
			Cluster.Add(Representative);
			Assigned[Representative] = true;
			for (const TPair<int32, double>& Link : Links[Representative])
			{
				if (!Assigned[Link.Key])
				{
					Assigned[Link.Key] = true;
					Cluster.Add(Link.Key);
					for (const int32 RepUnit : Classes[Representative])
					{
						for (const int32 MemberUnit : Classes[Link.Key])
						{
							AddUnitPair(RepUnit, MemberUnit);
						}
					}
				}
			}
			Algo::Sort(Cluster, [&](const int32 Left, const int32 Right) { return Left < Right; });

			FGroup Group;
			const FUnit& RepresentativeUnit = Units[Classes[Representative][0]];
			Group.Kind = RepresentativeUnit.Kind;
			Group.Name = RepresentativeUnit.Kind == UnitKind::WidgetTree ? FString() : RepresentativeUnit.Name;
			Group.Representative = Classes[Representative][0];
			Group.MinSimilarity = 1.0;
			Group.MaxSimilarity = 0.0;
			for (int32 Left = 0; Left < Cluster.Num(); ++Left)
			{
				Group.Members.Add(Classes[Cluster[Left]][0]);
				Group.NodeCount = FMath::Max(Group.NodeCount, Units[Classes[Cluster[Left]][0]].NodeCount);
				for (int32 Right = Left + 1; Right < Cluster.Num(); ++Right)
				{
					const double Similarity = Jaccard(Descriptors(Cluster[Left]), Descriptors(Cluster[Right]));
					Group.MinSimilarity = FMath::Min(Group.MinSimilarity, Similarity);
					Group.MaxSimilarity = FMath::Max(Group.MaxSimilarity, Similarity);
				}
			}
			SimilarGroups.Add(MoveTemp(Group));
		}
	}

	// Blueprint pairs from the deduplicated unit pairs: each pair of units counts once toward its Blueprint pair.
	// Widget-tree unit pairs only qualify the pair; the shared_graphs count is graphs, macros and event chains.
	TMap<TPair<int32, int32>, int32> Pairs;
	for (const TPair<int32, int32>& UnitPair : SharedUnitPairs)
	{
		const int32 LeftBlueprint = Units[UnitPair.Key].BlueprintIndex;
		const int32 RightBlueprint = Units[UnitPair.Value].BlueprintIndex;
		int32& Shared = Pairs.FindOrAdd({FMath::Min(LeftBlueprint, RightBlueprint), FMath::Max(LeftBlueprint, RightBlueprint)});
		if (Units[UnitPair.Key].Kind != UnitKind::WidgetTree)
		{
			++Shared;
		}
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
	for (const TPair<TPair<int32, int32>, int32>& Pair : Pairs)
	{
		double WidgetSimilarity = 0.0;
		const int32* LeftWidget = WidgetUnitOfBlueprint.Find(Pair.Key.Key);
		const int32* RightWidget = WidgetUnitOfBlueprint.Find(Pair.Key.Value);
		if (LeftWidget && RightWidget)
		{
			WidgetSimilarity = Jaccard(Units[*LeftWidget].Descriptors, Units[*RightWidget].Descriptors);
		}
		PairRows.Add({BlueprintPaths[Pair.Key.Key], BlueprintPaths[Pair.Key.Value], Pair.Value, WidgetSimilarity});
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
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("blueprints_scanned"), BlueprintPaths.Num());
	Result->SetNumberField(TEXT("graphs_scanned"), GraphsScanned);
	Result->SetNumberField(TEXT("min_nodes"), Request.MinNodes);
	Result->SetNumberField(TEXT("similarity_threshold"), Request.Similarity);
	CortexReflectAssetEnumeration::WriteMatchedFields(Enumeration, *Result);
	CortexReflectAssetEnumeration::WriteLoadFailures(LoadFailures, *Result);
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

	if (Request.bHasOutPath)
	{
		return CortexReflectAssetEnumeration::WriteReportWithSummary(
			Request.OutPath, Result, {TEXT("exact_groups"), TEXT("similar_groups"), TEXT("blueprint_pairs")});
	}
	return FCortexCommandRouter::Success(Result);
}
