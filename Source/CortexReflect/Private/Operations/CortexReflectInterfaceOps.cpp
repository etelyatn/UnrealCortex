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
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Operations/CortexReflectAssetEnumeration.h"
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
		for (const UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (!Graph)
			{
				continue;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
				if (!Event || Event->EventReference.GetMemberName() != Function->GetFName()
					|| !IsInterfaceClass(Event->EventReference.GetMemberParentClass(Blueprint->GeneratedClass), InterfaceAuthority))
				{
					continue;
				}
				const UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
				Result.Implementation = TEXT("event");
				Result.GraphName = Graph->GetName();
				Result.GraphGuid = Graph->GraphGuid;
				Result.EntryNodeGuid = Event->NodeGuid;
				Result.bHasBody = Then && Then->LinkedTo.Num() > 0;
				return Result;
			}
		}
		return Result;
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

	TSharedRef<FJsonObject> Result = InterfaceDescriptionJson(Interface);
	Result->SetStringField(TEXT("interface"), InterfaceName);
	Result->SetArrayField(TEXT("implementers"), ImplementerValues(Implementers, FunctionFilter));
	Result->SetNumberField(TEXT("implementer_count"), Implementers.Num());
	CortexReflectAssetEnumeration::WriteMatchedFields(Enumeration, *Result);
	CortexReflectAssetEnumeration::WriteLoadFailures(LoadFailures, *Result);

	if (bHasOutPath)
	{
		return CortexReflectAssetEnumeration::WriteReportWithSummary(OutPath, Result, {TEXT("implementers")});
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
