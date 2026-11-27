#include "Operations/CortexReflectInterfaceOps.h"

#include "Algo/Sort.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Message.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Operations/CortexReflectAssetEnumeration.h"
#include "Operations/CortexReflectNodeRefs.h"
#include "Operations/CortexReflectOps.h"
#include "UObject/Class.h"
#include "UObject/Interface.h"

namespace CortexReflectInterfacePrivate
{
	constexpr int32 MaxAncestorDepth = 64;

	FTopLevelAssetPath ClassPathFromTag(const FAssetData& Asset, const FName Tag)
	{
		FString Value;
		if (!Asset.GetTagValue(Tag, Value) || Value.IsEmpty())
		{
			return FTopLevelAssetPath();
		}
		return FSoftObjectPath(FPackageName::ExportTextPathToObjectPath(Value)).GetAssetPath();
	}

	/** The Blueprint asset that generated ClassPath (`/Game/X/BP_A.BP_A_C` -> `/Game/X/BP_A.BP_A`), if the registry knows it. */
	FAssetData FindAssetOfGeneratedClass(IAssetRegistry& Registry, const FTopLevelAssetPath& ClassPath)
	{
		FString AssetName = ClassPath.GetAssetName().ToString();
		if (!AssetName.RemoveFromEnd(TEXT("_C")))
		{
			return FAssetData();
		}
		return Registry.GetAssetByObjectPath(FSoftObjectPath(FTopLevelAssetPath(ClassPath.GetPackageName(), FName(*AssetName)), FString()));
	}

	FString GuidString(const FGuid& Guid)
	{
		return Guid.ToString();
	}

	bool IsInterfaceClass(const UClass* Class, const UClass* InterfaceAuthority)
	{
		return Class && Class->GetAuthoritativeClass() == InterfaceAuthority;
	}

	FCortexInterfaceFunctionImplementation Analyze(
		UBlueprint* Blueprint, const FBPInterfaceDescription& Description, const UFunction* Function, const UClass* InterfaceAuthority)
	{
		FCortexInterfaceFunctionImplementation Result;
		Result.Function = Function->GetFName();

		// An interface function implemented as a graph lives in the description's own graph list, not in FunctionGraphs.
		for (const UEdGraph* Graph : Description.Graphs)
		{
			if (Graph && Graph->GetFName() == Function->GetFName())
			{
				Result.Implementation = TEXT("function_graph");
				Result.GraphName = Graph->GetName();
				Result.GraphGuid = Graph->GraphGuid;
				Result.NodeCount = Graph->Nodes.Num();
				for (const UEdGraphNode* Node : Graph->Nodes)
				{
					if (Cast<UK2Node_FunctionEntry>(Node))
					{
						Result.EntryNodeGuid = Node->NodeGuid;
						break;
					}
				}
				return Result;
			}
		}

		// Event-style: an event node whose parent resolves (in the Blueprint's class context, as the engine does) to the interface.
		// It may sit below the top level of an event graph (collapsed into a composite graph), so every nested graph is searched.
		bool bFound = false;
		CortexReflectNodeRefs::ForEachNode(Blueprint,
			[&](const UEdGraph* TopGraph, const FString& SubgraphPath, const UEdGraphNode* Node)
			{
				const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
				if (bFound || !Event || !Blueprint->UbergraphPages.Contains(TopGraph) || Event->EventReference.GetMemberName() != Function->GetFName()
					|| !IsInterfaceClass(Event->EventReference.GetMemberParentClass(Blueprint->GeneratedClass), InterfaceAuthority))
				{
					return;
				}
				const UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
				bFound = true;
				Result.Implementation = TEXT("event");
				Result.GraphName = TopGraph->GetName();
				Result.GraphGuid = TopGraph->GraphGuid;
				Result.SubgraphPath = SubgraphPath;
				Result.EntryNodeGuid = Event->NodeGuid;
				Result.bHasBody = Then && Then->LinkedTo.Num() > 0;
			});
		return Result;
	}

	/** True when Blueprint itself implements Function without declaring the interface: an event node on it or an override function graph. */
	bool ImplementsOwn(UBlueprint* Blueprint, const UFunction* Function, const UClass* InterfaceAuthority)
	{
		const FBPInterfaceDescription NoDescription;
		if (Analyze(Blueprint, NoDescription, Function, InterfaceAuthority).Implementation != TEXT("none"))
		{
			return true;
		}
		return Blueprint->FunctionGraphs.ContainsByPredicate(
			[Function](const UEdGraph* Graph) { return Graph && Graph->GetFName() == Function->GetFName(); });
	}
}

TArray<FString> FCortexReflectInterfaceOps::GetImplementedInterfacePaths(const FAssetData& Asset)
{
	TArray<FString> Paths;
	const FAssetTagValueRef Tag = Asset.TagsAndValues.FindTag(FBlueprintTags::ImplementedInterfaces);
	if (!Tag.IsSet())
	{
		return Paths;
	}

	// The tag is the exported ImplementedInterfaces array: "((Interface=<class path>,...),(Interface=...))" where a class
	// path may be quoted and prefixed by its class ("/Script/CoreUObject.Class'/Game/X.BPI_C'").
	static const FString InterfaceIdentifier(TEXT("Interface="));
	FString Rest = Tag.AsString();
	int32 Start = Rest.Find(InterfaceIdentifier);
	while (Start != INDEX_NONE)
	{
		Rest.RightChopInline(Start + InterfaceIdentifier.Len());
		int32 End = INDEX_NONE;
		for (int32 Index = 0; Index < Rest.Len(); ++Index)
		{
			const TCHAR Char = Rest[Index];
			if (Char == TEXT(',') || Char == TEXT(')'))
			{
				End = Index;
				break;
			}
		}
		FString Token = End == INDEX_NONE ? Rest : Rest.Left(End);
		const int32 Quote = Token.Find(TEXT("'"));
		if (Quote != INDEX_NONE)
		{
			Token.RightChopInline(Quote + 1);
		}
		Token.RemoveFromStart(TEXT("\""));
		while (Token.EndsWith(TEXT("'")) || Token.EndsWith(TEXT("\"")))
		{
			Token.LeftChopInline(1);
		}
		if (!Token.IsEmpty())
		{
			Paths.Add(Token);
		}
		Start = Rest.Find(InterfaceIdentifier);
	}
	return Paths;
}

bool FCortexReflectInterfaceOps::TagImplementsInterface(const FAssetData& Asset, const UClass* Interface)
{
	if (!Interface)
	{
		return false;
	}
	const FString InterfacePath = Interface->GetAuthoritativeClass()->GetClassPathName().ToString();
	for (const FString& Path : GetImplementedInterfacePaths(Asset))
	{
		if (Path.Equals(InterfacePath, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

UBlueprint* FCortexReflectInterfaceOps::LoadBlueprint(const FAssetData& Asset, FString& OutError)
{
	const FString ObjectPath = Asset.GetSoftObjectPath().ToString();
	if (UBlueprint* Loaded = FindObject<UBlueprint>(nullptr, *ObjectPath))
	{
		return Loaded;
	}
	const FString PackageName = Asset.PackageName.ToString();
	if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
	{
		OutError = FString::Printf(TEXT("%s: package does not exist on disk"), *ObjectPath);
		return nullptr;
	}
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("%s: could not be loaded"), *ObjectPath);
	}
	return Blueprint;
}

TArray<const UFunction*> FCortexReflectInterfaceOps::GetInterfaceFunctions(const UClass* Interface)
{
	TArray<const UFunction*> Functions;
	if (!Interface)
	{
		return Functions;
	}
	for (TFieldIterator<UFunction> It(Interface, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		const UFunction* Function = *It;
		const UClass* Owner = Function->GetOwnerClass();
		if (!Owner || Owner == UInterface::StaticClass() || Owner == UObject::StaticClass()
			|| Function->GetName().StartsWith(TEXT("ExecuteUbergraph")))
		{
			continue;
		}
		Functions.Add(Function);
	}
	Algo::SortBy(Functions, [](const UFunction* Function) { return Function->GetFName().ToString(); });
	return Functions;
}

TSharedRef<FJsonObject> FCortexInterfaceFunctionImplementation::ToJson() const
{
	using namespace CortexReflectInterfacePrivate;
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("function"), Function.ToString());
	Json->SetStringField(TEXT("implementation"), Implementation);
	if (Implementation != TEXT("none"))
	{
		Json->SetStringField(TEXT("graph_name"), GraphName);
		Json->SetStringField(TEXT("graph_guid"), GuidString(GraphGuid));
		Json->SetStringField(TEXT("entry_node_guid"), GuidString(EntryNodeGuid));
	}
	if (Implementation == TEXT("function_graph"))
	{
		Json->SetNumberField(TEXT("node_count"), NodeCount);
	}
	else if (Implementation == TEXT("event"))
	{
		if (!SubgraphPath.IsEmpty())
		{
			Json->SetStringField(TEXT("subgraph_path"), SubgraphPath);
		}
		Json->SetBoolField(TEXT("has_body"), bHasBody);
	}
	return Json;
}

TSharedRef<FJsonObject> FCortexInterfaceImplementer::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("asset_path"), AssetPath);
	Json->SetStringField(TEXT("class_path"), ClassPath);
	Json->SetBoolField(TEXT("direct"), bDirect);
	if (!bDirect)
	{
		Json->SetStringField(TEXT("via"), Via);
	}
	else
	{
		TArray<TSharedPtr<FJsonValue>> FunctionValues;
		for (const FCortexInterfaceFunctionImplementation& Function : Functions)
		{
			FunctionValues.Add(MakeShared<FJsonValueObject>(Function.ToJson()));
		}
		Json->SetArrayField(TEXT("functions"), FunctionValues);
	}
	return Json;
}

void FCortexReflectInterfaceOps::CollectImplementers(
	UClass* Interface,
	const TArray<FAssetData>& Candidates,
	TArray<FCortexInterfaceImplementer>& OutImplementers,
	TArray<FString>& OutLoadFailures)
{
	using namespace CortexReflectInterfacePrivate;

	OutImplementers.Reset();
	if (!Interface)
	{
		return;
	}
	const UClass* InterfaceAuthority = Interface->GetAuthoritativeClass();
	const TArray<const UFunction*> InterfaceFunctions = GetInterfaceFunctions(InterfaceAuthority);
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Registry-only pass: which candidates declare the interface, and the class path of every candidate for the parent walk.
	TMap<FTopLevelAssetPath, FAssetData> ByClassPath;
	for (const FAssetData& Asset : Candidates)
	{
		const FTopLevelAssetPath ClassPath = ClassPathFromTag(Asset, FBlueprintTags::GeneratedClassPath);
		if (ClassPath.IsValid())
		{
			ByClassPath.Add(ClassPath, Asset);
		}
	}
	TMap<FTopLevelAssetPath, FAssetData> OutsideCache;
	const auto FindByClassPath = [&](const FTopLevelAssetPath& ClassPath) -> FAssetData
	{
		if (const FAssetData* Known = ByClassPath.Find(ClassPath))
		{
			return *Known;
		}
		if (const FAssetData* Cached = OutsideCache.Find(ClassPath))
		{
			return *Cached;
		}
		return OutsideCache.Add(ClassPath, FindAssetOfGeneratedClass(Registry, ClassPath));
	};

	for (const FAssetData& Asset : Candidates)
	{
		FCortexInterfaceImplementer Record;
		Record.AssetPath = Asset.GetObjectPathString();
		Record.ClassPath = ClassPathFromTag(Asset, FBlueprintTags::GeneratedClassPath).ToString();

		if (TagImplementsInterface(Asset, InterfaceAuthority))
		{
			FString LoadError;
			UBlueprint* Blueprint = LoadBlueprint(Asset, LoadError);
			if (!Blueprint)
			{
				OutLoadFailures.Add(LoadError);
				continue;
			}
			const FBPInterfaceDescription* Description = Blueprint->ImplementedInterfaces.FindByPredicate(
				[InterfaceAuthority](const FBPInterfaceDescription& Candidate)
				{
					return IsInterfaceClass(Candidate.Interface, InterfaceAuthority);
				});
			if (!Description)
			{
				OutLoadFailures.Add(FString::Printf(TEXT("%s: the registry tag names the interface but the loaded Blueprint does not implement it"), *Record.AssetPath));
				continue;
			}
			for (const UFunction* Function : InterfaceFunctions)
			{
				Record.Functions.Add(Analyze(Blueprint, *Description, Function, InterfaceAuthority));
			}
			OutImplementers.Add(MoveTemp(Record));
			continue;
		}

		// Not declared here: it implements the interface only if an ancestor Blueprint does (registry parent chain, no load).
		FTopLevelAssetPath ParentPath = ClassPathFromTag(Asset, FBlueprintTags::ParentClassPath);
		for (int32 Depth = 0; Depth < MaxAncestorDepth && ParentPath.IsValid(); ++Depth)
		{
			const FAssetData Parent = FindByClassPath(ParentPath);
			if (!Parent.IsValid())
			{
				break;
			}
			if (TagImplementsInterface(Parent, InterfaceAuthority))
			{
				Record.bDirect = false;
				Record.Via = Parent.GetObjectPathString();
				OutImplementers.Add(MoveTemp(Record));
				break;
			}
			ParentPath = ClassPathFromTag(Parent, FBlueprintTags::ParentClassPath);
		}
	}

	Algo::SortBy(OutImplementers, &FCortexInterfaceImplementer::AssetPath,
		[](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
}

TSharedRef<FJsonObject> FCortexInterfaceCaller::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("asset_path"), AssetPath);
	Json->SetStringField(TEXT("graph_name"), GraphName);
	Json->SetStringField(TEXT("graph_guid"), GraphGuid.ToString());
	Json->SetStringField(TEXT("subgraph_path"), SubgraphPath);
	Json->SetStringField(TEXT("node_guid"), NodeGuid.ToString());
	Json->SetStringField(TEXT("node_class"), NodeClass);
	Json->SetStringField(TEXT("kind"), Kind);
	if (!Function.IsEmpty())
	{
		Json->SetStringField(TEXT("function"), Function);
	}
	return Json;
}

namespace CortexReflectInterfacePrivate
{
	/** Packages that hard-reference a content interface's package; empty for a native interface (it has no content package). */
	TSet<FName> HardReferencerPackages(const UClass* InterfaceAuthority)
	{
		TSet<FName> Packages;
		if (InterfaceAuthority->ClassGeneratedBy == nullptr)
		{
			return Packages;
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FName> ReferencerNames;
		Registry.GetReferencers(InterfaceAuthority->GetOutermost()->GetFName(), ReferencerNames,
			UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard));
		Packages.Append(ReferencerNames);
		return Packages;
	}

	struct FCallerScan
	{
		const UClass* InterfaceAuthority = nullptr;
		TSet<FName> InterfaceFunctions;
		FString FunctionFilter;
		TArray<FCortexInterfaceCaller>* Callers = nullptr;
		TArray<FCortexInterfaceCaller>* Unresolved = nullptr;

		void ScanBlueprint(const UBlueprint* Blueprint, const FString& AssetPath) const
		{
			const UClass* Context = Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->SkeletonGeneratedClass.Get();
			CortexReflectNodeRefs::ForEachNode(Blueprint,
				[&](const UEdGraph* TopGraph, const FString& SubgraphPath, const UEdGraphNode* Node)
				{
					FCortexInterfaceCaller Site;
					Site.AssetPath = AssetPath;
					Site.GraphName = TopGraph->GetName();
					Site.GraphGuid = TopGraph->GraphGuid;
					Site.SubgraphPath = SubgraphPath;
					Site.NodeGuid = Node->NodeGuid;
					Site.NodeClass = CortexReflectNodeRefs::GetNodeClassLabel(Node);

					// Classified once: a message node derives from the call node, so it is tested first.
					if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
					{
						const FName FunctionName = Call->FunctionReference.GetMemberName();
						Site.Kind = Call->IsA<UK2Node_Message>() ? TEXT("message") : TEXT("typed_call");
						Site.Function = FunctionName.ToString();
						const bool bWanted = FunctionFilter.IsEmpty() || Site.Function == FunctionFilter;
						if (const UClass* Owner = CortexReflectNodeRefs::GetCallMemberOwner(Call, Context))
						{
							if (bWanted && Owner->IsChildOf(InterfaceAuthority))
							{
								Callers->Add(MoveTemp(Site));
							}
						}
						else if (bWanted && InterfaceFunctions.Contains(FunctionName))
						{
							Unresolved->Add(MoveTemp(Site));
						}
					}
					else if (const UClass* CastTarget = CortexReflectNodeRefs::GetCastTargetClass(Node))
					{
						if (CastTarget->GetAuthoritativeClass()->IsChildOf(InterfaceAuthority))
						{
							Site.Kind = TEXT("interface_cast");
							Callers->Add(MoveTemp(Site));
						}
					}
				});
		}
	};

	bool CallerLess(const FCortexInterfaceCaller& A, const FCortexInterfaceCaller& B)
	{
		const int32 Asset = A.AssetPath.Compare(B.AssetPath, ESearchCase::CaseSensitive);
		if (Asset != 0)
		{
			return Asset < 0;
		}
		const int32 Graph = A.GraphName.Compare(B.GraphName, ESearchCase::CaseSensitive);
		if (Graph != 0)
		{
			return Graph < 0;
		}
		const int32 Sub = A.SubgraphPath.Compare(B.SubgraphPath, ESearchCase::CaseSensitive);
		if (Sub != 0)
		{
			return Sub < 0;
		}
		return A.NodeGuid.ToString().Compare(B.NodeGuid.ToString(), ESearchCase::CaseSensitive) < 0;
	}
}

void FCortexReflectInterfaceOps::CollectCallers(
	UClass* Interface,
	const TArray<FAssetData>& Candidates,
	const FString& FunctionFilter,
	TArray<FCortexInterfaceCaller>& OutCallers,
	TArray<FCortexInterfaceCaller>& OutUnresolved,
	int32& OutScannedAssets,
	TArray<FString>& OutLoadFailures)
{
	using namespace CortexReflectInterfacePrivate;

	OutCallers.Reset();
	OutUnresolved.Reset();
	OutScannedAssets = 0;
	if (!Interface)
	{
		return;
	}

	FCallerScan Scan;
	Scan.InterfaceAuthority = Interface->GetAuthoritativeClass();
	for (const UFunction* Function : GetInterfaceFunctions(Scan.InterfaceAuthority))
	{
		Scan.InterfaceFunctions.Add(Function->GetFName());
	}
	Scan.FunctionFilter = FunctionFilter;
	Scan.Callers = &OutCallers;
	Scan.Unresolved = &OutUnresolved;

	// A content interface is referenced (hard) by every Blueprint that holds a node on it; a native interface has no
	// content package, so every candidate has to be loaded.
	const bool bNarrow = Scan.InterfaceAuthority->ClassGeneratedBy != nullptr;
	const TSet<FName> Referencers = HardReferencerPackages(Scan.InterfaceAuthority);

	for (const FAssetData& Asset : Candidates)
	{
		if (bNarrow && !Referencers.Contains(Asset.PackageName))
		{
			continue;
		}
		FString LoadError;
		const UBlueprint* Blueprint = LoadBlueprint(Asset, LoadError);
		if (!Blueprint)
		{
			OutLoadFailures.Add(LoadError);
			continue;
		}
		++OutScannedAssets;
		Scan.ScanBlueprint(Blueprint, Asset.GetObjectPathString());
	}

	OutCallers.Sort(CallerLess);
	OutUnresolved.Sort(CallerLess);
}

namespace CortexReflectInterfacePrivate
{
	TSharedRef<FJsonObject> InterfaceDescriptionJson(UClass* Interface)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("interface_class"), Interface->GetAuthoritativeClass()->GetClassPathName().ToString());
		Json->SetBoolField(TEXT("native"), Interface->GetAuthoritativeClass()->ClassGeneratedBy == nullptr);
		TArray<TSharedPtr<FJsonValue>> FunctionValues;
		for (const UFunction* Function : FCortexReflectInterfaceOps::GetInterfaceFunctions(Interface->GetAuthoritativeClass()))
		{
			TSharedRef<FJsonObject> FunctionJson = MakeShared<FJsonObject>();
			FunctionJson->SetStringField(TEXT("name"), Function->GetName());
			FunctionJson->SetStringField(TEXT("style"), UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function) ? TEXT("event") : TEXT("function"));
			FunctionValues.Add(MakeShared<FJsonValueObject>(FunctionJson));
		}
		Json->SetArrayField(TEXT("functions"), FunctionValues);
		return Json;
	}

	TArray<TSharedPtr<FJsonValue>> ImplementerValues(const TArray<FCortexInterfaceImplementer>& Implementers, const FString& FunctionFilter)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FCortexInterfaceImplementer& Implementer : Implementers)
		{
			TSharedRef<FJsonObject> Json = Implementer.ToJson();
			if (!FunctionFilter.IsEmpty() && Implementer.bDirect)
			{
				TArray<TSharedPtr<FJsonValue>> Kept;
				for (const FCortexInterfaceFunctionImplementation& Function : Implementer.Functions)
				{
					if (Function.Function.ToString() == FunctionFilter)
					{
						Kept.Add(MakeShared<FJsonValueObject>(Function.ToJson()));
					}
				}
				Json->SetArrayField(TEXT("functions"), Kept);
			}
			Values.Add(MakeShared<FJsonValueObject>(Json));
		}
		return Values;
	}

	bool IsInAnyPrefix(const FString& PackageName, const TArray<FString>& Prefixes)
	{
		return Prefixes.ContainsByPredicate(
			[&PackageName](const FString& Prefix)
			{
				return PackageName == Prefix || (PackageName.StartsWith(Prefix) && PackageName.Len() > Prefix.Len() && PackageName[Prefix.Len()] == TEXT('/'));
			});
	}

	/**
	 * `referencers`: {implements, calls_or_messages, other} asset paths, each sorted. A content interface's universe is the hard
	 * registry referencers of its package under Prefixes, classified by the implementer tag (a direct implementer in Implementers)
	 * and by the resolved caller sites in Callers (an asset can be in both). A native interface has no content package, so
	 * the buckets hold only the implementers and caller assets found and `other` is empty.
	 */
	TSharedRef<FJsonObject> ReferencersJson(
		const UClass* InterfaceAuthority,
		const TArray<FString>& Prefixes,
		const TArray<FCortexInterfaceImplementer>& Implementers,
		const TArray<FCortexInterfaceCaller>& Callers)
	{
		// Evidence grouped by package: a package can hold several top-level assets, and each proven asset goes to its own bucket.
		TMap<FName, TArray<FString>> ImplementsByPackage;
		TMap<FName, TArray<FString>> CallsByPackage;
		const auto AddEvidence = [](TMap<FName, TArray<FString>>& ByPackage, const FString& AssetPath)
		{
			ByPackage.FindOrAdd(FName(*FPackageName::ObjectPathToPackageName(AssetPath))).AddUnique(AssetPath);
		};
		for (const FCortexInterfaceImplementer& Implementer : Implementers)
		{
			if (Implementer.bDirect)
			{
				AddEvidence(ImplementsByPackage, Implementer.AssetPath);
			}
		}
		for (const FCortexInterfaceCaller& Site : Callers)
		{
			AddEvidence(CallsByPackage, Site.AssetPath);
		}

		TArray<FString> Implements;
		TArray<FString> CallsOrMessages;
		TArray<FString> Other;
		if (InterfaceAuthority->ClassGeneratedBy != nullptr)
		{
			IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			const FName InterfacePackage = InterfaceAuthority->GetOutermost()->GetFName();
			for (const FName Package : HardReferencerPackages(InterfaceAuthority))
			{
				const FString PackageName = Package.ToString();
				if (Package == InterfacePackage || !IsInAnyPrefix(PackageName, Prefixes))
				{
					continue;
				}
				const TArray<FString>* ImplementsHere = ImplementsByPackage.Find(Package);
				const TArray<FString>* CallsHere = CallsByPackage.Find(Package);
				if (ImplementsHere)
				{
					Implements.Append(*ImplementsHere);
				}
				if (CallsHere)
				{
					CallsOrMessages.Append(*CallsHere);
				}
				if (!ImplementsHere && !CallsHere)
				{
					// No evidence names an asset of this package: the package's primary asset is the unclassified referencer.
					TArray<FAssetData> Assets;
					Registry.GetAssetsByPackageName(Package, Assets);
					const FAssetData* Primary = Assets.FindByPredicate([](const FAssetData& Asset) { return Asset.IsTopLevelAsset(); });
					Other.Add(Primary ? Primary->GetObjectPathString() : PackageName);
				}
			}
		}
		else
		{
			for (const TPair<FName, TArray<FString>>& Entry : ImplementsByPackage)
			{
				Implements.Append(Entry.Value);
			}
			for (const TPair<FName, TArray<FString>>& Entry : CallsByPackage)
			{
				CallsOrMessages.Append(Entry.Value);
			}
		}

		const auto PathLess = [](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; };
		Algo::Sort(Implements, PathLess);
		Algo::Sort(CallsOrMessages, PathLess);
		Algo::Sort(Other, PathLess);

		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetArrayField(TEXT("implements"), CortexReflectAssetEnumeration::StringValues(Implements));
		Json->SetArrayField(TEXT("calls_or_messages"), CortexReflectAssetEnumeration::StringValues(CallsOrMessages));
		Json->SetArrayField(TEXT("other"), CortexReflectAssetEnumeration::StringValues(Other));
		return Json;
	}
}

UClass* FCortexReflectInterfaceOps::ResolveInterface(const FString& Name, FCortexCommandResult& OutError)
{
	UClass* Class = FCortexReflectOps::FindClassByName(Name, OutError);
	if (!Class)
	{
		return nullptr;
	}
	if (!Class->HasAnyClassFlags(CLASS_Interface))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("'%s' is not an interface class"), *Name));
		return nullptr;
	}
	return Class;
}

FCortexCommandResult FCortexReflectInterfaceOps::FindInterfaceImplementers(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexReflectInterfacePrivate;

	FString InterfaceName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("interface"), InterfaceName) || InterfaceName.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("interface must be a non-empty string"));
	}
	TArray<FString> Prefixes;
	FCortexCommandResult RequestError;
	if (!CortexReflectAssetEnumeration::ParsePathPrefixes(Params, Prefixes, RequestError))
	{
		return RequestError;
	}
	FString FunctionFilter;
	if (Params->HasField(TEXT("function")) && (!Params->TryGetStringField(TEXT("function"), FunctionFilter) || FunctionFilter.IsEmpty()))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("function must be a non-empty string"));
	}
	FString ImplementerFilter;
	const bool bHasImplementer = Params->HasField(TEXT("implementer"));
	if (bHasImplementer
		&& (!Params->HasTypedField<EJson::String>(TEXT("implementer")) || !Params->TryGetStringField(TEXT("implementer"), ImplementerFilter) || ImplementerFilter.IsEmpty()))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("implementer must be a non-empty Blueprint asset path string"));
	}
	bool bIncludeCallers = true;
	if (Params->HasField(TEXT("include_callers")) && !Params->TryGetBoolField(TEXT("include_callers"), bIncludeCallers))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("include_callers must be a boolean"));
	}
	bool bHasOutPath = false;
	FCortexResolvedFilePath OutPath;
	if (!CortexReflectAssetEnumeration::ParseOptionalOutPath(Params, bHasOutPath, OutPath, RequestError))
	{
		return RequestError;
	}

	UClass* Interface = ResolveInterface(InterfaceName, RequestError);
	if (!Interface)
	{
		return RequestError;
	}
	if (!FunctionFilter.IsEmpty()
		&& !GetInterfaceFunctions(Interface->GetAuthoritativeClass()).ContainsByPredicate(
			[&FunctionFilter](const UFunction* Function) { return Function->GetName() == FunctionFilter; }))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("'%s' is not a function of %s"), *FunctionFilter, *Interface->GetName()));
	}

	const CortexReflectAssetEnumeration::FEnumeration Enumeration =
		CortexReflectAssetEnumeration::Enumerate(Prefixes, {UBlueprint::StaticClass()->GetClassPathName()});
	TArray<FCortexInterfaceImplementer> Implementers;
	TArray<FString> LoadFailures;
	CollectImplementers(Interface, Enumeration.Assets, Implementers, LoadFailures);

	// The implementer parameter narrows the listing only; the referencers split classifies against every implementer.
	TArray<FCortexInterfaceImplementer> Listed = Implementers;
	if (bHasImplementer)
	{
		Listed.RemoveAll([&ImplementerFilter](const FCortexInterfaceImplementer& Implementer)
			{
				return !Implementer.AssetPath.Equals(ImplementerFilter, ESearchCase::IgnoreCase);
			});
	}

	TSharedRef<FJsonObject> Result = InterfaceDescriptionJson(Interface);
	Result->SetStringField(TEXT("interface"), InterfaceName);
	Result->SetArrayField(TEXT("implementers"), ImplementerValues(Listed, FunctionFilter));
	Result->SetNumberField(TEXT("implementer_count"), Listed.Num());
	if (bHasImplementer && Listed.Num() == 1)
	{
		// Per interface function: does the implementer handle it (a function graph or an event node) or leave it unimplemented.
		// An inherited implementer is resolved child first, then up its parent chain to the declaring ancestor (via): any level
		// that implements the function means the child executes an implementation. A proven positive is always reported true;
		// a negative needs every level inspected, so when a level could not be loaded or found the function is unknown and the
		// field is omitted. Levels that could not be inspected are listed in load_failures.
		TSet<FName> Handled;
		bool bChainInspected = true;
		const auto AddHandled = [&Handled](const TArray<FCortexInterfaceFunctionImplementation>& Functions)
		{
			for (const FCortexInterfaceFunctionImplementation& Function : Functions)
			{
				if (Function.Implementation != TEXT("none"))
				{
					Handled.Add(Function.Function);
				}
			}
		};
		TArray<FCortexInterfaceImplementer> ViaOutsideScope;
		if (Listed[0].bDirect)
		{
			AddHandled(Listed[0].Functions);
		}
		else
		{
			IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			const UClass* Authority = Interface->GetAuthoritativeClass();
			const TArray<const UFunction*> InterfaceFunctions = GetInterfaceFunctions(Authority);

			// The declaring ancestor: its interface description decides what it implements.
			const FCortexInterfaceImplementer* Via = Implementers.FindByPredicate(
				[&Listed](const FCortexInterfaceImplementer& Implementer) { return Implementer.bDirect && Implementer.AssetPath == Listed[0].Via; });
			if (!Via)
			{
				// The declaring ancestor lives outside path_prefixes: analyse it on its own.
				const FAssetData ViaAsset = Registry.GetAssetByObjectPath(FSoftObjectPath(Listed[0].Via));
				if (ViaAsset.IsValid())
				{
					CollectImplementers(Interface, {ViaAsset}, ViaOutsideScope, LoadFailures);
					Via = ViaOutsideScope.FindByPredicate([](const FCortexInterfaceImplementer& Implementer) { return Implementer.bDirect; });
				}
			}
			if (Via)
			{
				AddHandled(Via->Functions);
			}
			else
			{
				bChainInspected = false;
				LoadFailures.Add(FString::Printf(TEXT("%s: the declaring ancestor could not be analysed"), *Listed[0].Via));
			}

			// Every Blueprint below the declaring ancestor, the child first, may implement a function itself.
			FAssetData Level = Registry.GetAssetByObjectPath(FSoftObjectPath(Listed[0].AssetPath));
			bool bReachedVia = false;
			for (int32 Depth = 0; Depth < MaxAncestorDepth && Level.IsValid(); ++Depth)
			{
				if (Level.GetObjectPathString() == Listed[0].Via)
				{
					bReachedVia = true;
					break;
				}
				FString LoadError;
				if (UBlueprint* Blueprint = LoadBlueprint(Level, LoadError))
				{
					for (const UFunction* Function : InterfaceFunctions)
					{
						if (!Handled.Contains(Function->GetFName()) && ImplementsOwn(Blueprint, Function, Authority))
						{
							Handled.Add(Function->GetFName());
						}
					}
				}
				else
				{
					bChainInspected = false;
					LoadFailures.Add(LoadError);
				}
				const FTopLevelAssetPath ParentPath = ClassPathFromTag(Level, FBlueprintTags::ParentClassPath);
				Level = ParentPath.IsValid() ? FindAssetOfGeneratedClass(Registry, ParentPath) : FAssetData();
			}
			// The walk can end exactly on the declaring ancestor after its last allowed step.
			bReachedVia = bReachedVia || (Level.IsValid() && Level.GetObjectPathString() == Listed[0].Via);
			if (!bReachedVia)
			{
				// The registry lost a level on the way up (or the chain is implausibly deep): the levels above it are unseen.
				bChainInspected = false;
				LoadFailures.Add(FString::Printf(TEXT("%s: the parent chain could not be followed up to %s"), *Listed[0].AssetPath, *Listed[0].Via));
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* FunctionValues = nullptr;
		if (Result->TryGetArrayField(TEXT("functions"), FunctionValues))
		{
			for (const TSharedPtr<FJsonValue>& FunctionValue : *FunctionValues)
			{
				const TSharedPtr<FJsonObject> FunctionJson = FunctionValue->AsObject();
				const bool bHandled = Handled.Contains(FName(*FunctionJson->GetStringField(TEXT("name"))));
				if (bHandled || bChainInspected)
				{
					FunctionJson->SetBoolField(TEXT("implementer_handles"), bHandled);
				}
			}
		}
	}
	if (bIncludeCallers)
	{
		// Always scanned unfiltered: the referencers split classifies by every caller site; the function filter narrows the lists below.
		TArray<FCortexInterfaceCaller> Callers;
		TArray<FCortexInterfaceCaller> Unresolved;
		int32 ScannedAssets = 0;
		CollectCallers(Interface, Enumeration.Assets, FString(), Callers, Unresolved, ScannedAssets, LoadFailures);
		Result->SetObjectField(TEXT("referencers"), ReferencersJson(Interface->GetAuthoritativeClass(), Prefixes, Implementers, Callers));
		if (!FunctionFilter.IsEmpty())
		{
			const auto NotWanted = [&FunctionFilter](const FCortexInterfaceCaller& Site)
			{
				return Site.Kind != TEXT("interface_cast") && Site.Function != FunctionFilter;
			};
			Callers.RemoveAll(NotWanted);
			Unresolved.RemoveAll(NotWanted);
		}
		const auto ToValues = [](const TArray<FCortexInterfaceCaller>& Sites)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FCortexInterfaceCaller& Site : Sites)
			{
				Values.Add(MakeShared<FJsonValueObject>(Site.ToJson()));
			}
			return Values;
		};
		Result->SetArrayField(TEXT("callers"), ToValues(Callers));
		Result->SetNumberField(TEXT("caller_count"), Callers.Num());
		Result->SetArrayField(TEXT("unresolved_callers"), ToValues(Unresolved));
		Result->SetNumberField(TEXT("caller_assets_scanned"), ScannedAssets);
	}
	CortexReflectAssetEnumeration::WriteMatchedFields(Enumeration, *Result);
	CortexReflectAssetEnumeration::WriteLoadFailures(LoadFailures, *Result);

	if (bHasOutPath)
	{
		// The file keeps the full referencer buckets; the inline summary carries their counts (the lists can exceed the response budget).
		TSharedPtr<FJsonObject> Counts;
		const TSharedPtr<FJsonObject>* Buckets = nullptr;
		if (Result->TryGetObjectField(TEXT("referencers"), Buckets) && Buckets && Buckets->IsValid())
		{
			Counts = MakeShared<FJsonObject>();
			for (const TCHAR* Name : {TEXT("implements"), TEXT("calls_or_messages"), TEXT("other")})
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				(*Buckets)->TryGetArrayField(Name, Values);
				Counts->SetNumberField(FString(Name) + TEXT("_count"), Values ? Values->Num() : 0);
			}
		}
		FCortexCommandResult Written = CortexReflectAssetEnumeration::WriteReportWithSummary(OutPath, Result, {TEXT("implementers"), TEXT("callers"), TEXT("unresolved_callers")});
		if (Written.bSuccess && Written.Data.IsValid() && Counts.IsValid())
		{
			Written.Data->SetObjectField(TEXT("referencers"), Counts);
		}
		return Written;
	}
	return FCortexCommandRouter::Success(Result);
}

FCortexCommandResult FCortexReflectInterfaceOps::FindOverridesOfInterface(const TSharedPtr<FJsonObject>& Params, UClass* Interface)
{
	using namespace CortexReflectInterfacePrivate;

	TArray<FString> Prefixes = {TEXT("/Game")};
	if (Params.IsValid() && Params->HasField(TEXT("path_prefixes")))
	{
		FCortexCommandResult RequestError;
		if (!CortexReflectAssetEnumeration::ParsePathPrefixes(Params, Prefixes, RequestError))
		{
			return RequestError;
		}
	}

	const CortexReflectAssetEnumeration::FEnumeration Enumeration =
		CortexReflectAssetEnumeration::Enumerate(Prefixes, {UBlueprint::StaticClass()->GetClassPathName()});
	TArray<FCortexInterfaceImplementer> Implementers;
	TArray<FString> LoadFailures;
	CollectImplementers(Interface, Enumeration.Assets, Implementers, LoadFailures);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_name"), FString(Interface->GetPrefixCPP()) + Interface->GetName());
	Result->SetArrayField(TEXT("children"), TArray<TSharedPtr<FJsonValue>>());
	Result->SetNumberField(TEXT("total_overrides"), 0);
	Result->SetArrayField(TEXT("implementers"), ImplementerValues(Implementers, FString()));
	Result->SetNumberField(TEXT("implementer_count"), Implementers.Num());
	Result->SetArrayField(TEXT("path_prefixes"), CortexReflectAssetEnumeration::StringValues(Prefixes));
	Result->SetStringField(TEXT("note"),
		TEXT("An interface has no derived classes to override it; its implementing Blueprints are listed under implementers. "
			"reflect.find_interface_implementers adds the per-function implementation style and the callers."));
	CortexReflectAssetEnumeration::WriteLoadFailures(LoadFailures, *Result);
	return FCortexCommandRouter::Success(Result);
}
