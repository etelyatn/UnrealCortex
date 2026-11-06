#include "Operations/CortexReflectTypeUsageOps.h"

#include "Algo/Sort.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Engine/UserDefinedEnum.h"
#include "GameFramework/SaveGame.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_StructOperation.h"
#include "K2Node_SwitchEnum.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Operations/CortexReflectAssetEnumeration.h"
#include "Operations/CortexReflectInterfaceOps.h"
#include "Operations/CortexReflectNodeRefs.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/UObjectGlobals.h"

namespace CortexReflectTypeUsagePrivate
{
	/** Nodes scanned across all Blueprints; assets beyond it are reported as skipped, never counted as free of the type. */
	constexpr int32 MaxScannedNodes = 400000;

	struct FUsage
	{
		FString Category;
		FString AssetPath;
		FString GraphName;
		FGuid GraphGuid;
		FString SubgraphPath;
		FGuid NodeGuid;
		FString NodeClass;
		FString Pin;
		FString Direction;
		FString Variable;
		FString Container;
		FString Role;
		FString ViaClass;
		FString Member;
		FString AssetClass;

		TSharedRef<FJsonObject> ToJson() const
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("category"), Category);
			Json->SetStringField(TEXT("asset_path"), AssetPath);
			if (!NodeClass.IsEmpty())
			{
				Json->SetStringField(TEXT("graph_name"), GraphName);
				Json->SetStringField(TEXT("graph_guid"), GraphGuid.ToString());
				Json->SetStringField(TEXT("subgraph_path"), SubgraphPath);
				Json->SetStringField(TEXT("node_guid"), NodeGuid.ToString());
				Json->SetStringField(TEXT("node_class"), NodeClass);
			}
			SetIfSet(*Json, TEXT("pin"), Pin);
			SetIfSet(*Json, TEXT("direction"), Direction);
			SetIfSet(*Json, TEXT("variable"), Variable);
			SetIfSet(*Json, TEXT("container"), Container);
			SetIfSet(*Json, TEXT("role"), Role);
			SetIfSet(*Json, TEXT("via_class"), ViaClass);
			SetIfSet(*Json, TEXT("member"), Member);
			SetIfSet(*Json, TEXT("asset_class"), AssetClass);
			return Json;
		}

		FString SortKey() const
		{
			return FString::Join(TArray<FString>{AssetPath, Category, GraphName, SubgraphPath, NodeGuid.ToString(), Pin, Variable, Member}, TEXT("\n"));
		}

	private:
		static void SetIfSet(FJsonObject& Json, const TCHAR* Field, const FString& Value)
		{
			if (!Value.IsEmpty())
			{
				Json.SetStringField(Field, Value);
			}
		}
	};

	struct FTarget
	{
		UObject* Type = nullptr;
		UScriptStruct* Struct = nullptr;
		UEnum* Enum = nullptr;
		FString Path;
		bool bNative = false;
	};

	/** True when the pin type is, holds (container element) or maps to (map value) the target type. */
	bool RefsType(const FEdGraphPinType& PinType, const UObject* Type, FString& OutRole)
	{
		if (PinType.PinSubCategoryObject.Get() == Type)
		{
			OutRole = PinType.IsMap() ? TEXT("map_key") : TEXT("type");
			return true;
		}
		if (PinType.IsMap() && PinType.PinValueType.TerminalSubCategoryObject.Get() == Type)
		{
			OutRole = TEXT("map_value");
			return true;
		}
		return false;
	}

	FString ContainerName(const FEdGraphPinType& PinType)
	{
		switch (PinType.ContainerType)
		{
		case EPinContainerType::Array: return TEXT("array");
		case EPinContainerType::Set: return TEXT("set");
		case EPinContainerType::Map: return TEXT("map");
		default: return FString();
		}
	}

	UObject* LoadAssetObject(const FAssetData& Asset, FString& OutError)
	{
		const FString ObjectPath = Asset.GetSoftObjectPath().ToString();
		if (UObject* Loaded = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Loaded;
		}
		const FString PackageName = Asset.PackageName.ToString();
		if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
		{
			OutError = FString::Printf(TEXT("%s: package does not exist on disk"), *ObjectPath);
			return nullptr;
		}
		UObject* Object = LoadObject<UObject>(nullptr, *ObjectPath);
		if (!Object)
		{
			OutError = FString::Printf(TEXT("%s: could not be loaded"), *ObjectPath);
		}
		return Object;
	}

	/** A UDS/UDE asset path (`/Game/X/UDS_Foo[.UDS_Foo]`), a `/Script/Module.Name` path, or a struct/enum name (F/E prefix optional). */
	UObject* ResolveType(const FString& Name, FCortexCommandResult& OutError)
	{
		if (Name.StartsWith(TEXT("/Script/")))
		{
			if (UObject* Native = FindObject<UScriptStruct>(nullptr, *Name))
			{
				return Native;
			}
			if (UObject* Native = FindObject<UEnum>(nullptr, *Name))
			{
				return Native;
			}
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::ClassNotFound, FString::Printf(TEXT("Native struct or enum not found: %s"), *Name));
			return nullptr;
		}
		if (Name.StartsWith(TEXT("/")))
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(Name);
			if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound, FString::Printf(TEXT("Asset not found: %s"), *Name));
				return nullptr;
			}
			const FString ObjectPath = Name.Contains(TEXT(".")) ? Name : FString::Printf(TEXT("%s.%s"), *Name, *FPackageName::GetShortName(Name));
			UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath);
			if (!Asset)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound, FString::Printf(TEXT("Asset could not be loaded: %s"), *Name));
			}
			return Asset;
		}

		TArray<FString> Candidates = {Name};
		if (Name.Len() > 1 && (Name[0] == TEXT('F') || Name[0] == TEXT('E')))
		{
			Candidates.Add(Name.Mid(1));
		}
		for (const FString& Candidate : Candidates)
		{
			if (UObject* Found = FindFirstObject<UScriptStruct>(*Candidate, EFindFirstObjectOptions::NativeFirst))
			{
				return Found;
			}
			if (UObject* Found = FindFirstObject<UEnum>(*Candidate, EFindFirstObjectOptions::NativeFirst))
			{
				return Found;
			}
		}
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::ClassNotFound, FString::Printf(TEXT("Struct or enum not found: %s"), *Name));
		return nullptr;
	}

	void ScanBlueprint(const UBlueprint* Blueprint, const FString& AssetPath, const FTarget& Target, TArray<FUsage>& OutUsages, int32& InOutNodes)
	{
		const bool bSaveGame = Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(USaveGame::StaticClass());
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			FString Role;
			if (!RefsType(Variable.VarType, Target.Type, Role))
			{
				continue;
			}
			FUsage Usage;
			Usage.Category = TEXT("variable");
			Usage.AssetPath = AssetPath;
			Usage.Variable = Variable.VarName.ToString();
			Usage.Container = ContainerName(Variable.VarType);
			Usage.Role = Role;
			OutUsages.Add(Usage);
			if (bSaveGame)
			{
				Usage.Category = TEXT("save_game");
				OutUsages.Add(Usage);
			}
		}

		CortexReflectNodeRefs::ForEachNode(Blueprint,
			[&](const UEdGraph* TopGraph, const FString& SubgraphPath, const UEdGraphNode* Node)
			{
				++InOutNodes;
				const auto Make = [&](const TCHAR* Category)
				{
					FUsage Usage;
					Usage.Category = Category;
					Usage.AssetPath = AssetPath;
					Usage.GraphName = TopGraph->GetName();
					Usage.GraphGuid = TopGraph->GraphGuid;
					Usage.SubgraphPath = SubgraphPath;
					Usage.NodeGuid = Node->NodeGuid;
					Usage.NodeClass = CortexReflectNodeRefs::GetNodeClassLabel(Node);
					return Usage;
				};

				// Struct and enum nodes that are the type itself are reported as such, not as one pin entry per pin.
				if (const UK2Node_StructOperation* Operation = Cast<UK2Node_StructOperation>(Node))
				{
					if (Target.Struct && Operation->StructType == Target.Struct)
					{
						const TCHAR* Category = Operation->IsA<UK2Node_SetFieldsInStruct>() ? TEXT("set_fields")
							: Operation->IsA<UK2Node_MakeStruct>() ? TEXT("make_struct")
							: Operation->IsA<UK2Node_BreakStruct>() ? TEXT("break_struct")
							: TEXT("struct_member");
						OutUsages.Add(Make(Category));
						return;
					}
				}
				if (const UK2Node_SwitchEnum* Switch = Cast<UK2Node_SwitchEnum>(Node))
				{
					if (Target.Enum && Switch->Enum == Target.Enum)
					{
						OutUsages.Add(Make(TEXT("switch_enum")));
						return;
					}
				}
				if (const UK2Node_EnumLiteral* Literal = Cast<UK2Node_EnumLiteral>(Node))
				{
					if (Target.Enum && Literal->Enum == Target.Enum)
					{
						OutUsages.Add(Make(TEXT("enum_literal")));
						return;
					}
				}

				if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
				{
					for (const FBPVariableDescription& Local : Entry->LocalVariables)
					{
						FString Role;
						if (RefsType(Local.VarType, Target.Type, Role))
						{
							FUsage Usage = Make(TEXT("local_variable"));
							Usage.Variable = Local.VarName.ToString();
							Usage.Container = ContainerName(Local.VarType);
							Usage.Role = Role;
							OutUsages.Add(Usage);
						}
					}
				}
				// Function entry/result and custom event pins are the function's parameters; their node pins mirror them.
				if (const UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(Node))
				{
					for (const TSharedPtr<FUserPinInfo>& Info : Editable->UserDefinedPins)
					{
						FString Role;
						if (Info.IsValid() && RefsType(Info->PinType, Target.Type, Role))
						{
							FUsage Usage = Make(TEXT("function_parameter"));
							Usage.Pin = Info->PinName.ToString();
							Usage.Direction = Info->DesiredPinDirection == EGPD_Input ? TEXT("input") : TEXT("output");
							Usage.Container = ContainerName(Info->PinType);
							Usage.Role = Role;
							OutUsages.Add(Usage);
						}
					}
					return;
				}

				for (const UEdGraphPin* Pin : Node->Pins)
				{
					FString Role;
					if (Pin && RefsType(Pin->PinType, Target.Type, Role))
					{
						FUsage Usage = Make(TEXT("pin"));
						Usage.Pin = Pin->PinName.ToString();
						Usage.Direction = Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output");
						Usage.Container = ContainerName(Pin->PinType);
						Usage.Role = Role;
						OutUsages.Add(Usage);
					}
				}
			});
	}

	/** Members of a user-defined struct that are the target type. */
	void ScanStruct(const UUserDefinedStruct* Struct, const FString& AssetPath, const FTarget& Target, TArray<FUsage>& OutUsages)
	{
		for (const FStructVariableDescription& Member : FStructureEditorUtils::GetVarDesc(Struct))
		{
			const bool bDirect = Member.SubCategoryObject.ToSoftObjectPath().ToString() == Target.Path;
			const bool bMapValue = Member.ContainerType == EPinContainerType::Map && Member.PinValueType.TerminalSubCategoryObject.Get() == Target.Type;
			if (bDirect || bMapValue)
			{
				FUsage Usage;
				Usage.Category = TEXT("nested_struct");
				Usage.AssetPath = AssetPath;
				Usage.Member = Member.FriendlyName;
				Usage.Role = bDirect ? (Member.ContainerType == EPinContainerType::Map ? TEXT("map_key") : TEXT("type")) : TEXT("map_value");
				OutUsages.Add(Usage);
			}
		}
	}

	/** True when the package is a prefix folder or lies below one of the prefixes. */
	bool PathInPrefixes(const FString& PackageName, const TArray<FString>& Prefixes)
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
}

FCortexCommandResult FCortexReflectTypeUsageOps::FindTypeUsages(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexReflectTypeUsagePrivate;

	FString UserType;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("user_type"), UserType) || UserType.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("user_type must be a non-empty string"));
	}
	TArray<FString> Prefixes;
	FCortexCommandResult RequestError;
	if (!CortexReflectAssetEnumeration::ParsePathPrefixes(Params, Prefixes, RequestError))
	{
		return RequestError;
	}
	bool bHasOutPath = false;
	FCortexResolvedFilePath OutPath;
	if (!CortexReflectAssetEnumeration::ParseOptionalOutPath(Params, bHasOutPath, OutPath, RequestError))
	{
		return RequestError;
	}

	FTarget Target;
	Target.Type = ResolveType(UserType, RequestError);
	if (!Target.Type)
	{
		return RequestError;
	}
	Target.Struct = Cast<UScriptStruct>(Target.Type);
	Target.Enum = Cast<UEnum>(Target.Type);
	if (!Target.Struct && !Target.Enum)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, FString::Printf(TEXT("'%s' is neither a struct nor an enum"), *UserType));
	}
	Target.Path = Target.Type->GetPathName();
	Target.bNative = Target.Type->GetOutermost()->GetName().StartsWith(TEXT("/Script/"));

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	for (const FString& Prefix : Prefixes)
	{
		Registry.ScanPathsSynchronous({Prefix}, true);
	}

	// Asset classes a hard referencer can have.
	TSet<FTopLevelAssetPath> BlueprintClasses;
	Registry.GetDerivedClassNames({UBlueprint::StaticClass()->GetClassPathName()}, {}, BlueprintClasses);
	BlueprintClasses.Add(UBlueprint::StaticClass()->GetClassPathName());
	TSet<FTopLevelAssetPath> DataAssetClasses;
	Registry.GetDerivedClassNames({UDataAsset::StaticClass()->GetClassPathName()}, {}, DataAssetClasses);
	DataAssetClasses.Add(UDataAsset::StaticClass()->GetClassPathName());
	const FTopLevelAssetPath StructAssetClass = UUserDefinedStruct::StaticClass()->GetClassPathName();
	const FTopLevelAssetPath EnumAssetClass = UUserDefinedEnum::StaticClass()->GetClassPathName();
	const FTopLevelAssetPath DataTableClass = UDataTable::StaticClass()->GetClassPathName();

	TArray<FName> ReferencerPackages;
	Registry.GetReferencers(Target.Type->GetOutermost()->GetFName(), ReferencerPackages,
		UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard));
	ReferencerPackages.RemoveAll([&](const FName Package)
	{
		return Package == Target.Type->GetOutermost()->GetFName() || !PathInPrefixes(Package.ToString(), Prefixes);
	});
	ReferencerPackages.Sort(FNameLexicalLess());

	TArray<FAssetData> BlueprintAssets;
	TArray<FAssetData> StructAssets;
	TMap<FString, FUsage> DataAssets;
	TArray<FString> OtherReferencers;
	for (const FName Package : ReferencerPackages)
	{
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(Package, Assets, true);
		for (const FAssetData& Asset : Assets)
		{
			if (BlueprintClasses.Contains(Asset.AssetClassPath))
			{
				BlueprintAssets.Add(Asset);
			}
			else if (Asset.AssetClassPath == StructAssetClass)
			{
				StructAssets.Add(Asset);
			}
			else if (Asset.AssetClassPath == EnumAssetClass)
			{
				// A user-defined enum holds no type references.
			}
			else if (Asset.AssetClassPath == DataTableClass)
			{
				// Tables whose row structure is the type are reported from the tag below; any other table referencing it is not classified.
				FString RowStructure;
				if (!Asset.GetTagValue(FName(TEXT("RowStructure")), RowStructure) || RowStructure != Target.Path)
				{
					OtherReferencers.Add(Asset.GetObjectPathString());
				}
			}
			else if (DataAssetClasses.Contains(Asset.AssetClassPath))
			{
				FUsage Usage;
				Usage.Category = TEXT("data_asset");
				Usage.AssetPath = Asset.GetObjectPathString();
				Usage.AssetClass = Asset.AssetClassPath.ToString();
				DataAssets.Add(Usage.AssetPath, Usage);
			}
			else
			{
				OtherReferencers.Add(Asset.GetObjectPathString());
			}
		}
	}

	TArray<FUsage> Usages;
	TArray<FString> LoadFailures;
	TArray<FString> SkippedAssets;
	int32 ScannedNodes = 0;
	int32 ScannedAssets = 0;

	// DataTables: the row structure is an Asset Registry tag, so no table is loaded.
	if (Target.Struct)
	{
		FARFilter TableFilter;
		TableFilter.ClassPaths.Add(DataTableClass);
		TableFilter.bRecursiveClasses = true;
		for (const FString& Prefix : Prefixes)
		{
			TableFilter.PackagePaths.Add(FName(*Prefix));
		}
		TableFilter.bRecursivePaths = true;
		TArray<FAssetData> Tables;
		Registry.GetAssets(TableFilter, Tables);
		for (const FAssetData& Table : Tables)
		{
			FString RowStructure;
			if (Table.GetTagValue(FName(TEXT("RowStructure")), RowStructure) && RowStructure == Target.Path)
			{
				FUsage Usage;
				Usage.Category = TEXT("datatable_row_struct");
				Usage.AssetPath = Table.GetObjectPathString();
				Usage.AssetClass = Table.AssetClassPath.ToString();
				Usages.Add(Usage);
			}
		}
	}

	// Blueprints: loaded and scanned; instances of a Blueprint DataAsset class that uses the type are found through the class.
	for (const FAssetData& Asset : BlueprintAssets)
	{
		if (ScannedNodes >= MaxScannedNodes)
		{
			SkippedAssets.Add(FString::Printf(TEXT("%s: node budget of %d exhausted"), *Asset.GetObjectPathString(), MaxScannedNodes));
			continue;
		}
		FString LoadError;
		const UBlueprint* Blueprint = FCortexReflectInterfaceOps::LoadBlueprint(Asset, LoadError);
		if (!Blueprint)
		{
			LoadFailures.Add(LoadError);
			continue;
		}
		++ScannedAssets;
		const int32 FirstNew = Usages.Num();
		ScanBlueprint(Blueprint, Asset.GetObjectPathString(), Target, Usages, ScannedNodes);

		const bool bUsesType = Usages.Num() > FirstNew;
		if (bUsesType && Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(UDataAsset::StaticClass()))
		{
			FARFilter InstanceFilter;
			InstanceFilter.ClassPaths.Add(Blueprint->GeneratedClass->GetClassPathName());
			InstanceFilter.bRecursiveClasses = true;
			for (const FString& Prefix : Prefixes)
			{
				InstanceFilter.PackagePaths.Add(FName(*Prefix));
			}
			InstanceFilter.bRecursivePaths = true;
			TArray<FAssetData> Instances;
			Registry.GetAssets(InstanceFilter, Instances);
			for (const FAssetData& Instance : Instances)
			{
				FUsage Usage;
				Usage.Category = TEXT("data_asset");
				Usage.AssetPath = Instance.GetObjectPathString();
				Usage.AssetClass = Instance.AssetClassPath.ToString();
				Usage.ViaClass = Blueprint->GeneratedClass->GetClassPathName().ToString();
				DataAssets.Add(Usage.AssetPath, Usage);
			}
		}
	}
	for (const TPair<FString, FUsage>& Pair : DataAssets)
	{
		Usages.Add(Pair.Value);
	}

	// Other user-defined structs with a member of the type.
	for (const FAssetData& Asset : StructAssets)
	{
		FString LoadError;
		UObject* Loaded = LoadAssetObject(Asset, LoadError);
		if (const UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Loaded))
		{
			++ScannedAssets;
			ScanStruct(Struct, Asset.GetObjectPathString(), Target, Usages);
		}
		else
		{
			LoadFailures.Add(LoadError.IsEmpty() ? FString::Printf(TEXT("%s: is not a user-defined struct"), *Asset.GetObjectPathString()) : LoadError);
		}
	}

	Algo::SortBy(Usages, &FUsage::SortKey, [](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	OtherReferencers.Sort();

	TSharedRef<FJsonObject> ByCategory = MakeShared<FJsonObject>();
	TMap<FString, int32> Counts;
	TArray<TSharedPtr<FJsonValue>> UsageValues;
	for (const FUsage& Usage : Usages)
	{
		++Counts.FindOrAdd(Usage.Category);
		UsageValues.Add(MakeShared<FJsonValueObject>(Usage.ToJson()));
	}
	for (const TPair<FString, int32>& Count : Counts)
	{
		ByCategory->SetNumberField(Count.Key, Count.Value);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("user_type"), UserType);
	Result->SetStringField(TEXT("type_path"), Target.Path);
	Result->SetStringField(TEXT("kind"), Target.Struct ? TEXT("struct") : TEXT("enum"));
	Result->SetBoolField(TEXT("native"), Target.bNative);
	Result->SetArrayField(TEXT("usages"), UsageValues);
	Result->SetNumberField(TEXT("usage_count"), Usages.Num());
	Result->SetObjectField(TEXT("by_category"), ByCategory);
	Result->SetNumberField(TEXT("referencer_packages"), ReferencerPackages.Num());
	Result->SetNumberField(TEXT("assets_scanned"), ScannedAssets);
	Result->SetNumberField(TEXT("nodes_scanned"), ScannedNodes);
	Result->SetArrayField(TEXT("other_referencers"), CortexReflectAssetEnumeration::StringValues(OtherReferencers));
	if (SkippedAssets.Num() > 0)
	{
		Result->SetArrayField(TEXT("skipped_assets"), CortexReflectAssetEnumeration::StringValues(SkippedAssets));
	}
	Result->SetArrayField(TEXT("notes"), CortexReflectAssetEnumeration::StringValues({
		TEXT("Hard referencers of the type's package are classified; other_referencers lists referencing assets of classes this command does not classify (worlds, materials, animation, ...), which are not scanned."),
		TEXT("SaveGame slot files (.sav) are outside Content and are not scanned; save_game lists the Blueprint classes that declare the type."),
		TEXT("Enum equality, for-each-enum and enumerator-name nodes are reported as pin entries of their typed pins.")}));
	CortexReflectAssetEnumeration::WriteLoadFailures(LoadFailures, *Result);

	if (bHasOutPath)
	{
		return CortexReflectAssetEnumeration::WriteReportWithSummary(OutPath, Result, {TEXT("usages"), TEXT("other_referencers")});
	}
	return FCortexCommandRouter::Success(Result);
}
