#include "Operations/CortexReflectDependencyGraphOps.h"

#include "Algo/Sort.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "CortexReflectModule.h"
#include "CortexSafeFileContract.h"
#include "Operations/CortexReflectAssetEnumeration.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/World.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Message.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Operations/CortexReflectConfigScan.h"
#include "Operations/CortexReflectNodeRefs.h"
#include "Operations/CortexReflectOps.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/GarbageCollection.h"

namespace CortexReflectDependencyGraphPrivate
{
	namespace EdgeKind
	{
		const TCHAR* const Inherits = TEXT("inherits");
		const TCHAR* const Implements = TEXT("implements");
		const TCHAR* const CallsMessage = TEXT("calls_message");
		const TCHAR* const Casts = TEXT("casts");
		const TCHAR* const Spawns = TEXT("spawns");
		const TCHAR* const CreatesWidget = TEXT("creates_widget");
		const TCHAR* const WidgetTreeChild = TEXT("widget_tree_child");
		const TCHAR* const ListViewEntry = TEXT("listview_entry");
		const TCHAR* const LevelPlacement = TEXT("level_placement");
		const TCHAR* const StructUse = TEXT("struct_use");
		const TCHAR* const EnumUse = TEXT("enum_use");
		const TCHAR* const Config = TEXT("config");
	}

	namespace NodeKind
	{
		const TCHAR* const Blueprint = TEXT("blueprint");
		const TCHAR* const World = TEXT("world");
		const TCHAR* const Struct = TEXT("struct");
		const TCHAR* const Enum = TEXT("enum");
		const TCHAR* const Asset = TEXT("asset");
		const TCHAR* const NativeClass = TEXT("native_class");
		const TCHAR* const ConfigFile = TEXT("config_file");
	}

	const TArray<FString>& GetEdgeKindNames()
	{
		static const TArray<FString> Names = {
			EdgeKind::Inherits, EdgeKind::Implements, EdgeKind::CallsMessage, EdgeKind::Casts, EdgeKind::Spawns,
			EdgeKind::CreatesWidget, EdgeKind::WidgetTreeChild, EdgeKind::ListViewEntry, EdgeKind::LevelPlacement,
			EdgeKind::StructUse, EdgeKind::EnumUse, EdgeKind::Config
		};
		return Names;
	}

	/** Registry entry of one enumerated asset. */
	struct FAssetEntry
	{
		const FAssetData* Asset;
		FString Id;
		FString Kind;
	};

	struct FRequest
	{
		TArray<FString> Prefixes;
		TSet<FString> EdgeKinds;
		/** Kinds that order the waves; hard kinds by default. */
		TSet<FString> OrderingKinds;
		FCortexResolvedFilePath OutPath;

		bool Wants(const TCHAR* Kind) const { return EdgeKinds.Contains(Kind); }

		/** True when any selected edge kind is read from loaded Blueprints rather than from Asset Registry data. */
		bool NeedsBlueprintLoad() const
		{
			return Wants(EdgeKind::Implements) || Wants(EdgeKind::CallsMessage) || Wants(EdgeKind::Casts)
				|| Wants(EdgeKind::Spawns) || Wants(EdgeKind::CreatesWidget) || Wants(EdgeKind::WidgetTreeChild)
				|| Wants(EdgeKind::ListViewEntry);
		}
	};

	bool ParseRequest(const TSharedPtr<FJsonObject>& Params, FRequest& OutRequest, FCortexCommandResult& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
			return false;
		}

		if (!CortexReflectAssetEnumeration::ParsePathPrefixes(Params, OutRequest.Prefixes, OutError))
		{
			return false;
		}

		const TArray<FString>& Known = GetEdgeKindNames();
		if (Params->HasField(TEXT("edge_kinds")))
		{
			const TArray<TSharedPtr<FJsonValue>>* KindValues = nullptr;
			if (!Params->TryGetArrayField(TEXT("edge_kinds"), KindValues) || KindValues->Num() == 0)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					TEXT("edge_kinds must be a non-empty array of edge kind names"));
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *KindValues)
			{
				FString Kind;
				if (!Value.IsValid() || !Value->TryGetString(Kind) || !Known.Contains(Kind))
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("Unknown edge kind '%s' (valid: %s)"), *Kind, *FString::Join(Known, TEXT(", "))));
					return false;
				}
				OutRequest.EdgeKinds.Add(Kind);
			}
		}
		else
		{
			OutRequest.EdgeKinds.Append(Known);
		}

		// Ordering uses hard kinds unless the caller names others; soft kinds are still emitted as edges.
		if (Params->HasField(TEXT("ordering_kinds")))
		{
			const TArray<TSharedPtr<FJsonValue>>* OrderingValues = nullptr;
			if (!Params->TryGetArrayField(TEXT("ordering_kinds"), OrderingValues) || OrderingValues->Num() == 0)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					TEXT("ordering_kinds must be a non-empty array of edge kind names"));
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *OrderingValues)
			{
				FString Kind;
				if (!Value.IsValid() || !Value->TryGetString(Kind) || !Known.Contains(Kind))
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("Unknown ordering kind '%s' (valid: %s)"), *Kind, *FString::Join(Known, TEXT(", "))));
					return false;
				}
				OutRequest.OrderingKinds.Add(Kind);
			}
		}
		else
		{
			OutRequest.OrderingKinds = {
				EdgeKind::Inherits, EdgeKind::Implements, EdgeKind::StructUse, EdgeKind::EnumUse,
				EdgeKind::WidgetTreeChild, EdgeKind::ListViewEntry };
		}

		FString OutPath;
		if (!Params->TryGetStringField(TEXT("out_path"), OutPath) || OutPath.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: out_path"));
			return false;
		}
		// An unusable out_path is refused before any asset is touched.
		FString PathErrorCode;
		FString PathErrorMessage;
		if (!FCortexSafeFileContract::ResolveWritePath(OutPath, OutRequest.OutPath, PathErrorCode, PathErrorMessage))
		{
			OutError = FCortexCommandRouter::Error(PathErrorCode, PathErrorMessage);
			return false;
		}
		return true;
	}

	struct FNode
	{
		FString Id;
		FString Kind;
		bool bExternal = false;
	};

	struct FGraphEdge
	{
		FString From;
		FString To;
		FString Kind;
		/** Blueprint graph holding the node that produced the edge; empty for non-graph edges. */
		FString Graph;
		/** Widget (tree entry) name for widget_tree_child / listview_entry edges. */
		FString Widget;
	};

	int32 CompareIds(const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive);
	}

	/** Package name of an object path ("/Game/A/B.B" -> "/Game/A/B"); a package name is returned unchanged. */
	FString PackageOf(const FString& ObjectPath)
	{
		return FPackageName::ObjectPathToPackageName(ObjectPath);
	}

	/** "/Game/A/BP_X.BP_X_C" (or a SKEL_ class) -> "/Game/A/BP_X.BP_X"; native and non-class paths are unchanged. */
	FString NormaliseClassPath(const FString& ClassPath)
	{
		if (ClassPath.StartsWith(TEXT("/Script/")))
		{
			return ClassPath;
		}
		FString Package;
		FString Name;
		if (!ClassPath.Split(TEXT("."), &Package, &Name))
		{
			return ClassPath;
		}
		Name.RemoveFromStart(TEXT("SKEL_"));
		Name.RemoveFromEnd(TEXT("_C"));
		return Package + TEXT(".") + Name;
	}

	/** Blueprint-generated and skeleton classes resolve to their Blueprint asset; native classes keep their /Script path. */
	FString NormaliseClass(const UClass* Class)
	{
		if (!Class)
		{
			return FString();
		}
		if (const UObject* GeneratedBy = Class->ClassGeneratedBy)
		{
			return GeneratedBy->GetPathName();
		}
		return NormaliseClassPath(Class->GetPathName());
	}

	FString ClassifyAsset(const FAssetData& Asset)
	{
		if (Asset.IsInstanceOf(UBlueprint::StaticClass(), EResolveClass::Yes))
		{
			return NodeKind::Blueprint;
		}
		if (Asset.IsInstanceOf(UWorld::StaticClass(), EResolveClass::Yes))
		{
			return NodeKind::World;
		}
		if (Asset.IsInstanceOf(UUserDefinedStruct::StaticClass(), EResolveClass::Yes))
		{
			return NodeKind::Struct;
		}
		if (Asset.IsInstanceOf(UUserDefinedEnum::StaticClass(), EResolveClass::Yes))
		{
			return NodeKind::Enum;
		}
		return NodeKind::Asset;
	}

	/** Nodes and de-duplicated edges of the graph under construction. */
	class FGraph
	{
	public:
		explicit FGraph(const TArray<FString>& InPrefixes)
			: Prefixes(InPrefixes)
		{
		}

		bool IsInPrefix(const FString& PackageName) const
		{
			for (const FString& Prefix : Prefixes)
			{
				if (CortexReflectAssetEnumeration::IsInPrefix(PackageName, Prefix))
				{
					return true;
				}
			}
			return false;
		}

		/** Registers an asset or class target; targets outside the prefixes (and native classes) are flagged external. */
		FNode& EnsureNode(const FString& Id, const TCHAR* KindHint)
		{
			if (FNode* Existing = Nodes.Find(Id))
			{
				return *Existing;
			}
			FNode& Node = Nodes.Add(Id);
			Node.Id = Id;
			if (Id.StartsWith(TEXT("/Script/")))
			{
				Node.Kind = NodeKind::NativeClass;
				Node.bExternal = true;
			}
			else
			{
				Node.Kind = KindHint;
				Node.bExternal = !IsInPrefix(PackageOf(Id));
			}
			return Node;
		}

		void AddConfigFileNode(const FString& File)
		{
			if (!Nodes.Contains(File))
			{
				FNode& Node = Nodes.Add(File);
				Node.Id = File;
				Node.Kind = NodeKind::ConfigFile;
			}
		}

		void AddEdge(const FString& From, const FString& To, const TCHAR* Kind, const FString& Graph = FString(), const FString& Widget = FString())
		{
			if (From.IsEmpty() || To.IsEmpty() || From == To)
			{
				return;
			}
			const FString Key = FString::Join(TArray<FString>{From, To, Kind, Graph, Widget}, TEXT("|"));
			if (EdgeKeys.Contains(Key))
			{
				return;
			}
			EdgeKeys.Add(Key);
			Edges.Add({From, To, Kind, Graph, Widget});
		}

		TMap<FString, FNode> Nodes;
		TArray<FGraphEdge> Edges;

	private:
		const TArray<FString>& Prefixes;
		TSet<FString> EdgeKeys;
	};

	/** Kind and object path of the primary asset in a package, resolved once per package. */
	class FPackageLookup
	{
	public:
		struct FInfo
		{
			FString Kind;
			FString ObjectPath;
		};

		explicit FPackageLookup(IAssetRegistry& InRegistry)
			: Registry(InRegistry)
		{
		}

		void Seed(const FName PackageName, const FString& Kind, const FString& ObjectPath)
		{
			Cache.Add(PackageName, {Kind, ObjectPath});
		}

		/** Empty Kind when the package holds no registered asset. */
		const FInfo& Find(const FName PackageName)
		{
			if (const FInfo* Cached = Cache.Find(PackageName))
			{
				return *Cached;
			}
			FInfo Info;
			TArray<FAssetData> Assets;
			Registry.GetAssetsByPackageName(PackageName, Assets, /*bIncludeOnlyOnDiskAssets=*/true);
			for (const FAssetData& Asset : Assets)
			{
				const FString Kind = ClassifyAsset(Asset);
				if (Info.Kind.IsEmpty() || Kind != NodeKind::Asset)
				{
					Info.Kind = Kind;
					Info.ObjectPath = Asset.GetObjectPathString();
				}
			}
			return Cache.Add(PackageName, MoveTemp(Info));
		}

	private:
		IAssetRegistry& Registry;
		TMap<FName, FInfo> Cache;
	};

	UWidgetTree* GetWidgetTree(UBlueprint* Blueprint)
	{
		const FObjectProperty* Property = CastField<FObjectProperty>(Blueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
		return Property ? Cast<UWidgetTree>(Property->GetObjectPropertyValue_InContainer(Blueprint)) : nullptr;
	}

	struct FUnresolved
	{
		/** Blueprints whose parent class could not be resolved (no tag path, no loaded ParentClass). */
		int32 Parents = 0;
		/** Implemented-interface entries without a resolvable class. */
		int32 Interfaces = 0;
	};

	/** Edges read from a loaded Blueprint: interfaces, graph nodes and the widget tree. */
	void CollectLoadedBlueprintEdges(
		const FRequest& Request, const FString& Id, UBlueprint* Blueprint, FGraph& Graph, bool& bInOutHasParentEdge, FUnresolved& InOutUnresolved)
	{
		if (Request.Wants(EdgeKind::Inherits) && !bInOutHasParentEdge)
		{
			if (Blueprint->ParentClass)
			{
				Graph.EnsureNode(NormaliseClass(Blueprint->ParentClass), NodeKind::Blueprint);
				Graph.AddEdge(Id, NormaliseClass(Blueprint->ParentClass), EdgeKind::Inherits);
				bInOutHasParentEdge = true;
			}
			else
			{
				++InOutUnresolved.Parents;
			}
		}

		if (Request.Wants(EdgeKind::Implements))
		{
			for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
			{
				if (const UClass* InterfaceClass = Description.Interface)
				{
					const FString Target = NormaliseClass(InterfaceClass);
					Graph.EnsureNode(Target, NodeKind::Blueprint);
					Graph.AddEdge(Id, Target, EdgeKind::Implements);
				}
				else
				{
					++InOutUnresolved.Interfaces;
				}
			}
		}

		const auto AddNodeEdge = [&Graph, &Id](const UClass* TargetClass, const TCHAR* Kind, const FString& GraphName)
		{
			if (TargetClass)
			{
				const FString Target = NormaliseClass(TargetClass);
				Graph.EnsureNode(Target, NodeKind::Blueprint);
				Graph.AddEdge(Id, Target, Kind, GraphName);
			}
		};

		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (const UEdGraph* EdGraph : AllGraphs)
		{
			if (!EdGraph)
			{
				continue;
			}
			const FString GraphName = EdGraph->GetName();
			for (UEdGraphNode* Node : EdGraph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (Request.Wants(EdgeKind::CallsMessage))
				{
					if (const UK2Node_Message* Message = Cast<UK2Node_Message>(Node))
					{
						const UFunction* Function = Message->GetTargetFunction();
						AddNodeEdge(
							Function ? Function->GetOwnerClass() : Message->FunctionReference.GetMemberParentClass(Blueprint->GeneratedClass),
							EdgeKind::CallsMessage, GraphName);
					}
					else if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
					{
						// A typed call whose member lives on an interface class reaches every implementer of that interface.
						const UClass* MemberClass = Call->FunctionReference.GetMemberParentClass(Blueprint->GeneratedClass);
						if (MemberClass && MemberClass->HasAnyClassFlags(CLASS_Interface))
						{
							AddNodeEdge(MemberClass, EdgeKind::CallsMessage, GraphName);
						}
					}
				}
				if (Request.Wants(EdgeKind::Casts))
				{
					AddNodeEdge(CortexReflectNodeRefs::GetCastTargetClass(Node), EdgeKind::Casts, GraphName);
				}
				if (Request.Wants(EdgeKind::Spawns))
				{
					AddNodeEdge(CortexReflectNodeRefs::GetSpawnedClass(Node), EdgeKind::Spawns, GraphName);
				}
				if (Request.Wants(EdgeKind::CreatesWidget))
				{
					AddNodeEdge(CortexReflectNodeRefs::GetCreatedWidgetClass(Node), EdgeKind::CreatesWidget, GraphName);
				}
			}
		}

		if (Request.Wants(EdgeKind::WidgetTreeChild) || Request.Wants(EdgeKind::ListViewEntry))
		{
			if (const UWidgetTree* Tree = GetWidgetTree(Blueprint))
			{
				TArray<UWidget*> Widgets;
				Tree->GetAllWidgets(Widgets);
				for (const UWidget* Widget : Widgets)
				{
					if (!Widget)
					{
						continue;
					}
					const UClass* WidgetClass = Widget->GetClass();
					const FString WidgetName = Widget->GetName();
					if (Request.Wants(EdgeKind::WidgetTreeChild) && WidgetClass->ClassGeneratedBy)
					{
						const FString Target = NormaliseClass(WidgetClass);
						Graph.EnsureNode(Target, NodeKind::Blueprint);
						Graph.AddEdge(Id, Target, EdgeKind::WidgetTreeChild, FString(), WidgetName);
					}
					if (Request.Wants(EdgeKind::ListViewEntry))
					{
						const FClassProperty* EntryProperty = FindFProperty<FClassProperty>(WidgetClass, TEXT("EntryWidgetClass"));
						const UClass* EntryClass = EntryProperty ? Cast<UClass>(EntryProperty->GetObjectPropertyValue_InContainer(Widget)) : nullptr;
						if (EntryClass)
						{
							const FString Target = NormaliseClass(EntryClass);
							Graph.EnsureNode(Target, NodeKind::Blueprint);
							Graph.AddEdge(Id, Target, EdgeKind::ListViewEntry, FString(), WidgetName);
						}
					}
				}
			}
		}
	}

	/** Wave order over the schedulable nodes. Dependencies come first; each strongly connected component is one unit. */
	struct FSchedule
	{
		TArray<TArray<FString>> Waves;
		/** Strongly connected components with more than one member. */
		TArray<TArray<FString>> Cycles;
	};

	/**
	 * Condenses the dependency graph into strongly connected components (Tarjan, iterative) and places each
	 * component on its longest-path level: level 0 depends on nothing, otherwise 1 + the deepest dependency.
	 * All members of a cycle share a wave and everything downstream of a cycle lands in a strictly later wave.
	 */
	FSchedule BuildSchedule(const TArray<FString>& SortedIds, const TArray<FGraphEdge>& Edges, const TSet<FString>& DependencyKinds)
	{
		const int32 Count = SortedIds.Num();
		TMap<FString, int32> IndexOf;
		IndexOf.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			IndexOf.Add(SortedIds[Index], Index);
		}

		// Dependencies[i] = distinct nodes that i depends on (sorted for determinism).
		TArray<TArray<int32>> Dependencies;
		Dependencies.SetNum(Count);
		for (const FGraphEdge& Edge : Edges)
		{
			if (!DependencyKinds.Contains(Edge.Kind))
			{
				continue;
			}
			const int32* From = IndexOf.Find(Edge.From);
			const int32* To = IndexOf.Find(Edge.To);
			if (From && To && *From != *To)
			{
				Dependencies[*From].AddUnique(*To);
			}
		}
		for (TArray<int32>& Targets : Dependencies)
		{
			Targets.Sort();
		}

		TArray<int32> Order;
		Order.Init(INDEX_NONE, Count);
		TArray<int32> Low;
		Low.Init(0, Count);
		TArray<bool> OnStack;
		OnStack.Init(false, Count);
		TArray<int32> ComponentOf;
		ComponentOf.Init(INDEX_NONE, Count);
		TArray<int32> Stack;
		TArray<TArray<int32>> Components; // emitted dependencies-first
		int32 NextOrder = 0;
		struct FFrame
		{
			int32 Node;
			int32 Cursor;
		};

		for (int32 Root = 0; Root < Count; ++Root)
		{
			if (Order[Root] != INDEX_NONE)
			{
				continue;
			}
			TArray<FFrame> Frames;
			const auto Enter = [&](const int32 Node)
			{
				Order[Node] = Low[Node] = NextOrder++;
				Stack.Add(Node);
				OnStack[Node] = true;
				Frames.Add({Node, 0});
			};
			Enter(Root);
			while (Frames.Num() > 0)
			{
				FFrame& Frame = Frames.Last();
				if (Frame.Cursor < Dependencies[Frame.Node].Num())
				{
					const int32 Target = Dependencies[Frame.Node][Frame.Cursor++];
					if (Order[Target] == INDEX_NONE)
					{
						Enter(Target);
					}
					else if (OnStack[Target])
					{
						Low[Frame.Node] = FMath::Min(Low[Frame.Node], Order[Target]);
					}
					continue;
				}

				const int32 Node = Frame.Node;
				const int32 NodeLow = Low[Node];
				Frames.Pop();
				if (Frames.Num() > 0)
				{
					Low[Frames.Last().Node] = FMath::Min(Low[Frames.Last().Node], NodeLow);
				}
				if (Low[Node] == Order[Node])
				{
					TArray<int32>& Component = Components.AddDefaulted_GetRef();
					int32 Member;
					do
					{
						Member = Stack.Pop();
						OnStack[Member] = false;
						ComponentOf[Member] = Components.Num() - 1;
						Component.Add(Member);
					} while (Member != Node);
					Component.Sort();
				}
			}
		}

		// Components are emitted dependencies-first, so every dependency's level is known when it is needed.
		TArray<int32> Level;
		Level.Init(0, Components.Num());
		int32 MaxLevel = -1;
		for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
		{
			int32 Deepest = -1;
			for (const int32 Member : Components[ComponentIndex])
			{
				for (const int32 Target : Dependencies[Member])
				{
					if (ComponentOf[Target] != ComponentIndex)
					{
						Deepest = FMath::Max(Deepest, Level[ComponentOf[Target]]);
					}
				}
			}
			Level[ComponentIndex] = Deepest + 1;
			MaxLevel = FMath::Max(MaxLevel, Level[ComponentIndex]);
		}

		FSchedule Schedule;
		Schedule.Waves.SetNum(MaxLevel + 1);
		for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
		{
			for (const int32 Member : Components[ComponentIndex])
			{
				Schedule.Waves[Level[ComponentIndex]].Add(SortedIds[Member]);
			}
			if (Components[ComponentIndex].Num() > 1)
			{
				TArray<FString>& Cycle = Schedule.Cycles.AddDefaulted_GetRef();
				for (const int32 Member : Components[ComponentIndex])
				{
					Cycle.Add(SortedIds[Member]);
				}
			}
		}
		for (TArray<FString>& Wave : Schedule.Waves)
		{
			Algo::Sort(Wave, [](const FString& A, const FString& B) { return CompareIds(A, B) < 0; });
		}
		Algo::Sort(Schedule.Cycles, [](const TArray<FString>& A, const TArray<FString>& B) { return CompareIds(A[0], B[0]) < 0; });
		return Schedule;
	}

	TSharedRef<FJsonObject> NodeJson(const FNode& Node)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("id"), Node.Id);
		Json->SetStringField(TEXT("kind"), Node.Kind);
		if (Node.bExternal)
		{
			Json->SetBoolField(TEXT("external"), true);
		}
		return Json;
	}

	TSharedRef<FJsonObject> EdgeJson(const FGraphEdge& Edge)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("from"), Edge.From);
		Json->SetStringField(TEXT("to"), Edge.To);
		Json->SetStringField(TEXT("kind"), Edge.Kind);
		if (!Edge.Graph.IsEmpty())
		{
			Json->SetStringField(TEXT("graph"), Edge.Graph);
		}
		if (!Edge.Widget.IsEmpty())
		{
			Json->SetStringField(TEXT("widget"), Edge.Widget);
		}
		return Json;
	}

	using CortexReflectAssetEnumeration::StringValues;

	TArray<TSharedPtr<FJsonValue>> GroupValues(const TArray<TArray<FString>>& Groups)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TArray<FString>& Group : Groups)
		{
			Values.Add(MakeShared<FJsonValueArray>(StringValues(Group)));
		}
		return Values;
	}
}

FCortexCommandResult FCortexReflectDependencyGraphOps::ExportDependencyGraph(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexReflectDependencyGraphPrivate;

	FRequest Request;
	FCortexCommandResult RequestError;
	if (!ParseRequest(Params, Request, RequestError))
	{
		return RequestError;
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// One registry query enumerates every graph-relevant asset under the prefixes; nothing is loaded here.
	const CortexReflectAssetEnumeration::FEnumeration Enumeration = CortexReflectAssetEnumeration::Enumerate(
		Request.Prefixes,
		{
			UBlueprint::StaticClass()->GetClassPathName(),
			UWorld::StaticClass()->GetClassPathName(),
			UUserDefinedStruct::StaticClass()->GetClassPathName(),
			UUserDefinedEnum::StaticClass()->GetClassPathName()
		});

	FGraph Graph(Request.Prefixes);
	FPackageLookup Packages(Registry);
	TArray<FAssetEntry> Entries;
	Entries.Reserve(Enumeration.Assets.Num());
	for (const FAssetData& Asset : Enumeration.Assets)
	{
		FAssetEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Asset = &Asset;
		Entry.Id = Asset.GetObjectPathString();
		Entry.Kind = ClassifyAsset(Asset);
		Graph.EnsureNode(Entry.Id, *Entry.Kind);
		Packages.Seed(Asset.PackageName, Entry.Kind, Entry.Id);
	}

	const bool bNeedsLoad = Request.NeedsBlueprintLoad();
	const bool bNeedsDependencies = Request.Wants(EdgeKind::StructUse) || Request.Wants(EdgeKind::EnumUse) || Request.Wants(EdgeKind::LevelPlacement);
	const UE::AssetRegistry::FDependencyQuery HardQuery(UE::AssetRegistry::EDependencyQuery::Hard);
	bool bLoadedAny = false;
	TArray<FString> LoadFailures;
	FUnresolved Unresolved;

	for (const FAssetEntry& Entry : Entries)
	{
		const FAssetData& Asset = *Entry.Asset;
		const bool bIsBlueprint = Entry.Kind == NodeKind::Blueprint;

		// inherits: the registry's ParentClassPath tag, no load needed.
		bool bHasParentEdge = false;
		if (bIsBlueprint && Request.Wants(EdgeKind::Inherits))
		{
			const FAssetTagValueRef GeneratedTag = Asset.TagsAndValues.FindTag(FBlueprintTags::GeneratedClassPath);
			const FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag(FBlueprintTags::ParentClassPath);
			const FAssetTagValueRef NativeParentTag = Asset.TagsAndValues.FindTag(FBlueprintTags::NativeParentClassPath);
			FCortexBlueprintCatalogClassPaths ClassPaths;
			TArray<FString> InvalidFields;
			FCortexReflectOps::ParseBlueprintCatalogClassPaths(
				GeneratedTag.IsSet() ? GeneratedTag.AsString() : FString(),
				ParentTag.IsSet() ? ParentTag.AsString() : FString(),
				NativeParentTag.IsSet() ? NativeParentTag.AsString() : FString(),
				ClassPaths, InvalidFields);
			if (ClassPaths.ParentClassPath.IsValid())
			{
				const FString Parent = NormaliseClassPath(ClassPaths.ParentClassPath.ToString());
				Graph.EnsureNode(Parent, NodeKind::Blueprint);
				Graph.AddEdge(Entry.Id, Parent, EdgeKind::Inherits);
				bHasParentEdge = true;
			}
		}

		// Graph nodes, interfaces and widget trees need the loaded Blueprint (each loaded once).
		if (bIsBlueprint && (bNeedsLoad || (Request.Wants(EdgeKind::Inherits) && !bHasParentEdge)))
		{
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Entry.Id);
			if (Blueprint)
			{
				bLoadedAny = true;
				CollectLoadedBlueprintEdges(Request, Entry.Id, Blueprint, Graph, bHasParentEdge, Unresolved);
			}
			else
			{
				LoadFailures.Add(Entry.Id);
			}
		}

		// struct_use / enum_use / level_placement: hard package dependencies, classified once per package.
		if (bNeedsDependencies && Entry.Kind != NodeKind::Enum && Entry.Kind != NodeKind::Asset)
		{
			TArray<FName> Dependencies;
			Registry.GetDependencies(Asset.PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package, HardQuery);
			for (const FName Dependency : Dependencies)
			{
				if (Dependency == Asset.PackageName || Dependency.ToString().StartsWith(TEXT("/Script/")))
				{
					continue;
				}
				const FPackageLookup::FInfo& Target = Packages.Find(Dependency);
				const TCHAR* EdgeKindName = nullptr;
				if (Target.Kind == NodeKind::Struct && Entry.Kind != NodeKind::World)
				{
					EdgeKindName = EdgeKind::StructUse;
				}
				else if (Target.Kind == NodeKind::Enum && Entry.Kind != NodeKind::World)
				{
					EdgeKindName = EdgeKind::EnumUse;
				}
				else if (Target.Kind == NodeKind::Blueprint && Entry.Kind == NodeKind::World)
				{
					EdgeKindName = EdgeKind::LevelPlacement;
				}
				if (EdgeKindName && Request.Wants(EdgeKindName))
				{
					Graph.EnsureNode(Target.ObjectPath, *Target.Kind);
					Graph.AddEdge(Entry.Id, Target.ObjectPath, EdgeKindName);
				}
			}

			// level_placement with World Partition / One-File-Per-Actor: placed actors live in external actor packages that
			// the map package does not depend on. The packages under the World's __ExternalActors__ folder are enumerated
			// and every Blueprint they depend on is a placement in that map, whether or not it is under the prefixes
			// (an ordinary map's hard dependencies emit out-of-prefix Blueprints as external nodes the same way).
			if (Entry.Kind == NodeKind::World && Request.Wants(EdgeKind::LevelPlacement))
			{
				const FString ExternalActorsPath = ULevel::GetExternalActorsPath(Asset.PackageName.ToString());
				TArray<FAssetData> ActorAssets;
				if (!ExternalActorsPath.IsEmpty())
				{
					Registry.GetAssetsByPath(FName(*ExternalActorsPath), ActorAssets, /*bRecursive=*/true);
				}
				TSet<FName> ActorPackages;
				for (const FAssetData& ActorAsset : ActorAssets)
				{
					// Nested maps (/Maps/Arena and /Maps/Arena/Sub) share a folder prefix: the longest owning World wins.
					const FString ActorPackageName = ActorAsset.PackageName.ToString();
					FString Owner;
					for (const FAssetEntry& Other : Entries)
					{
						if (Other.Kind != NodeKind::World)
						{
							continue;
						}
						const FString OtherPackage = Other.Asset->PackageName.ToString();
						const FString OtherActors = ULevel::GetExternalActorsPath(OtherPackage);
						if (!OtherActors.IsEmpty() && ActorPackageName.StartsWith(OtherActors + TEXT("/")) && OtherPackage.Len() > Owner.Len())
						{
							Owner = OtherPackage;
						}
					}
					if (Owner == Asset.PackageName.ToString())
					{
						ActorPackages.Add(ActorAsset.PackageName);
					}
				}
				for (const FName ActorPackage : ActorPackages)
				{
					TArray<FName> ActorDependencies;
					Registry.GetDependencies(ActorPackage, ActorDependencies, UE::AssetRegistry::EDependencyCategory::Package, HardQuery);
					for (const FName ActorDependency : ActorDependencies)
					{
						if (ActorDependency.ToString().StartsWith(TEXT("/Script/")))
						{
							continue;
						}
						const FPackageLookup::FInfo& Placed = Packages.Find(ActorDependency);
						if (Placed.Kind == NodeKind::Blueprint)
						{
							Graph.EnsureNode(Placed.ObjectPath, *Placed.Kind);
							Graph.AddEdge(Entry.Id, Placed.ObjectPath, EdgeKind::LevelPlacement);
						}
					}
				}
			}
		}
	}

	// config: INI references to existing packages. The ini file is a node of its own; targets outside the
	// prefixes become external nodes, in-prefix targets that are not Blueprint/World/struct/enum are plain "asset" nodes.
	if (Request.Wants(EdgeKind::Config))
	{
		for (const CortexReflectConfigScan::FConfigReference& Reference : CortexReflectConfigScan::Scan())
		{
			if (Reference.bDirectory)
			{
				continue;
			}
			const FPackageLookup::FInfo& Target = Packages.Find(FName(*Reference.PackageName));
			if (Target.Kind.IsEmpty())
			{
				continue; // broken reference; find_broken_config_paths reports those
			}
			Graph.AddConfigFileNode(Reference.File);
			Graph.EnsureNode(Target.ObjectPath, *Target.Kind);
			Graph.AddEdge(Reference.File, Target.ObjectPath, EdgeKind::Config);
		}
	}

	if (bLoadedAny)
	{
		// Loaded Blueprints are not kept; one collection for the whole run.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}

	// Waves: Blueprint/World/struct/enum nodes under the prefixes; config files, plain assets and external targets are never scheduled.
	TArray<FString> SchedulableIds;
	for (const TPair<FString, FNode>& Pair : Graph.Nodes)
	{
		if (!Pair.Value.bExternal && Pair.Value.Kind != NodeKind::ConfigFile && Pair.Value.Kind != NodeKind::Asset)
		{
			SchedulableIds.Add(Pair.Key);
		}
	}
	Algo::Sort(SchedulableIds, [](const FString& A, const FString& B) { return CompareIds(A, B) < 0; });
	TSet<FString> DependencyKinds = Request.OrderingKinds.Intersect(Request.EdgeKinds);
	const FSchedule Schedule = BuildSchedule(SchedulableIds, Graph.Edges, DependencyKinds);

	TArray<FNode> SortedNodes;
	Graph.Nodes.GenerateValueArray(SortedNodes);
	Algo::SortBy(SortedNodes, &FNode::Id, [](const FString& A, const FString& B) { return CompareIds(A, B) < 0; });
	TArray<FGraphEdge> SortedEdges = Graph.Edges;
	Algo::Sort(SortedEdges, [](const FGraphEdge& A, const FGraphEdge& B)
	{
		if (const int32 Result = CompareIds(A.From, B.From)) { return Result < 0; }
		if (const int32 Result = CompareIds(A.To, B.To)) { return Result < 0; }
		if (const int32 Result = CompareIds(A.Kind, B.Kind)) { return Result < 0; }
		if (const int32 Result = CompareIds(A.Graph, B.Graph)) { return Result < 0; }
		return CompareIds(A.Widget, B.Widget) < 0;
	});

	TSharedRef<FJsonObject> EdgesByKind = MakeShared<FJsonObject>();
	for (const FGraphEdge& Edge : SortedEdges)
	{
		EdgesByKind->SetNumberField(Edge.Kind, EdgesByKind->HasField(Edge.Kind) ? EdgesByKind->GetNumberField(Edge.Kind) + 1 : 1);
	}

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (const FNode& Node : SortedNodes)
	{
		NodeValues.Add(MakeShared<FJsonValueObject>(NodeJson(Node)));
	}
	TArray<TSharedPtr<FJsonValue>> EdgeValues;
	for (const FGraphEdge& Edge : SortedEdges)
	{
		EdgeValues.Add(MakeShared<FJsonValueObject>(EdgeJson(Edge)));
	}

	TArray<FString> SortedKinds = Request.EdgeKinds.Array();
	Algo::Sort(SortedKinds, [](const FString& A, const FString& B) { return CompareIds(A, B) < 0; });
	TArray<FString> UsedOrderingKinds = DependencyKinds.Array();
	Algo::Sort(UsedOrderingKinds, [](const FString& A, const FString& B) { return CompareIds(A, B) < 0; });
	TSharedRef<FJsonObject> UnresolvedJson = MakeShared<FJsonObject>();
	UnresolvedJson->SetNumberField(TEXT("parents"), Unresolved.Parents);
	UnresolvedJson->SetNumberField(TEXT("interfaces"), Unresolved.Interfaces);
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetArrayField(TEXT("path_prefixes"), StringValues(Request.Prefixes));
	Report->SetArrayField(TEXT("edge_kinds"), StringValues(SortedKinds));
	Report->SetArrayField(TEXT("ordering_kinds"), StringValues(UsedOrderingKinds));
	Report->SetObjectField(TEXT("unresolved"), UnresolvedJson);
	Report->SetArrayField(TEXT("nodes"), NodeValues);
	Report->SetArrayField(TEXT("edges"), EdgeValues);
	Report->SetArrayField(TEXT("waves"), GroupValues(Schedule.Waves));
	Report->SetArrayField(TEXT("cycles"), GroupValues(Schedule.Cycles));
	CortexReflectAssetEnumeration::WriteMatchedFields(Enumeration, *Report);
	CortexReflectAssetEnumeration::WriteLoadFailures(LoadFailures, *Report);

	const FCortexJsonFileWriteResult Written = FCortexSafeFileContract::WriteJsonReportAtomic(Request.OutPath, Report);
	if (!Written.bWritten)
	{
		return FCortexCommandRouter::Error(Written.ErrorCode, Written.ErrorMessage);
	}

	TArray<TSharedPtr<FJsonValue>> WaveSizes;
	for (const TArray<FString>& Wave : Schedule.Waves)
	{
		WaveSizes.Add(MakeShared<FJsonValueNumber>(Wave.Num()));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("out_path"), Request.OutPath.AbsolutePath);
	Result->SetNumberField(TEXT("nodes"), SortedNodes.Num());
	Result->SetObjectField(TEXT("edges_by_kind"), EdgesByKind);
	Result->SetArrayField(TEXT("waves"), WaveSizes);
	Result->SetNumberField(TEXT("cycles"), Schedule.Cycles.Num());
	Result->SetArrayField(TEXT("ordering_kinds"), StringValues(UsedOrderingKinds));
	Result->SetObjectField(TEXT("unresolved"), UnresolvedJson);
	CortexReflectAssetEnumeration::WriteMatchedFields(Enumeration, *Result);
	CortexReflectAssetEnumeration::WriteLoadFailures(LoadFailures, *Result);
	UE_LOG(LogCortexReflect, Log, TEXT("export_dependency_graph: %d nodes, %d edges, %d waves, %d cycles -> %s"),
		SortedNodes.Num(), SortedEdges.Num(), Schedule.Waves.Num(), Schedule.Cycles.Num(), *Request.OutPath.AbsolutePath);
	return FCortexCommandRouter::Success(Result);
}
