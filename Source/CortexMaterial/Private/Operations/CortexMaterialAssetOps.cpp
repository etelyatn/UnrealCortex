#include "Operations/CortexMaterialAssetOps.h"
#include "Operations/CortexMaterialInputUtils.h"
#include "CortexMaterialModule.h"
#include "CortexEditorUtils.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Factories/MaterialFactoryNew.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/SavePackage.h"
#include "ObjectTools.h"
#include "CortexSerializer.h"
#include "CortexBatchScope.h"
#include "CortexPropertyUtils.h"
#include "Misc/MemStack.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

namespace
{
static constexpr int32 SM6_MAX_COLLECTION_REFERENCES = 2;

// Collect distinct MPC references from top-level material expressions.
static void CollectReferencedCollections(
	const UMaterial* Material,
	TArray<TSharedPtr<FJsonValue>>& OutCollections,
	TArray<TSharedPtr<FJsonValue>>& OutWarnings)
{
	if (!Material || !Material->GetEditorOnlyData())
	{
		return;
	}

	TSet<UMaterialParameterCollection*> UniqueCollections;
	for (UMaterialExpression* Expr : Material->GetEditorOnlyData()->ExpressionCollection.Expressions)
	{
		if (const UMaterialExpressionCollectionParameter* CollectionParam =
			Cast<UMaterialExpressionCollectionParameter>(Expr))
		{
			if (CollectionParam->Collection)
			{
				UniqueCollections.Add(CollectionParam->Collection);
			}
		}
	}

	TArray<UMaterialParameterCollection*> SortedCollections = UniqueCollections.Array();
	SortedCollections.Sort([](const UMaterialParameterCollection& A, const UMaterialParameterCollection& B)
	{
		return A.GetPathName() < B.GetPathName();
	});

	for (UMaterialParameterCollection* Collection : SortedCollections)
	{
		TSharedPtr<FJsonObject> ColObj = MakeShared<FJsonObject>();
		ColObj->SetStringField(TEXT("name"), Collection->GetName());
		ColObj->SetStringField(TEXT("asset_path"), Collection->GetPathName());
		OutCollections.Add(MakeShared<FJsonValueObject>(ColObj));
	}

	if (UniqueCollections.Num() > SM6_MAX_COLLECTION_REFERENCES)
	{
		OutWarnings.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("References %d MaterialParameterCollections - SM6 limit is %d"),
				UniqueCollections.Num(), SM6_MAX_COLLECTION_REFERENCES)));
	}
}

bool ResolveOwnedMaterialPath(UMaterial* Material, const FString& Path, UObject*& OutOwner,
	FProperty*& OutProperty, void*& OutAddress)
{
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("."), false);
	if (Segments.IsEmpty() || Segments.ContainsByPredicate([](const FString& Part) { return Part.IsEmpty(); }))
	{
		return false;
	}
	UObject* Root = Material;
	FString Relative = Path;
	if (Segments[0].Equals(TEXT("EditorOnlyData"), ESearchCase::IgnoreCase))
	{
		Root = Material->GetEditorOnlyData();
		if (!Root || Segments.Num() < 2)
		{
			return false;
		}
		Segments.RemoveAt(0);
		Relative = FString::Join(Segments, TEXT("."));
	}
	OutOwner = Root;
	UStruct* Type = Root->GetClass();
	void* Container = Root;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		FProperty* Field = Type->FindPropertyByName(*Segments[Index]);
		if (!Field || Field->ArrayDim != 1)
		{
			return false;
		}
		if (Index == Segments.Num() - 1)
		{
			break;
		}
		void* Address = Field->ContainerPtrToValuePtr<void>(Container);
		if (const auto* Struct = CastField<FStructProperty>(Field))
		{
			Type = Struct->Struct;
			Container = Address;
		}
		else if (const auto* Object = CastField<FObjectProperty>(Field))
		{
			UObject* Child = Object->GetObjectPropertyValue(Address);
			if (!Child || (Child != Material && !Child->IsIn(Material)))
			{
				return false;
			}
			OutOwner = Child;
			Type = Child->GetClass();
			Container = Child;
		}
		else
		{
			return false;
		}
	}
	return FCortexPropertyUtils::ResolvePropertyPath(Root, Relative, OutProperty, OutAddress);
}

// Reflect the member chain inside a serialized input struct that owns LeafProperty, e.g. UseConstant, Constant
// or Constant.R. Descent follows the resolved address; naming uses property identity, so a member that shares
// the struct's first address (Expression, Constant.R) is still reported as itself.
bool BuildInputLeafPath(const UStruct* Struct, void* Container, const void* Target, const FProperty* LeafProperty,
	FString& OutLeaf, int32 Depth = 0)
{
	if (!Struct || !LeafProperty || Depth > 3)
	{
		return false;
	}
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Field = *It;
		if (Field == LeafProperty)
		{
			OutLeaf = Field->GetName();
			return true;
		}
		const FStructProperty* StructField = CastField<FStructProperty>(Field);
		if (StructField && Field->ArrayDim == 1)
		{
			void* FieldAddress = Field->ContainerPtrToValuePtr<void>(Container);
			const uint8* Begin = static_cast<const uint8*>(FieldAddress);
			const uint8* Test = static_cast<const uint8*>(Target);
			if (Test >= Begin && Test < Begin + StructField->Struct->GetPropertiesSize())
			{
				FString Nested;
				if (!BuildInputLeafPath(StructField->Struct, FieldAddress, Target, LeafProperty, Nested, Depth + 1))
				{
					return false;
				}
				OutLeaf = Field->GetName() + TEXT(".") + Nested;
				return true;
			}
		}
	}
	return false;
}

// Detect protected root-input storage by the object identity the resolver actually reached. A whole-input
// target yields an empty leaf so callers can refuse expression/whole-root replacement; any owned route that
// lands on a representable leaf is canonicalized through the same typed patch as the direct form.
bool ResolveRootInputLeaf(UMaterial* Material, const FProperty* LeafProperty, void* Address,
	EMaterialProperty& OutProperty, FString& OutLeaf)
{
	UMaterialEditorOnlyData* EditorData = Material ? Material->GetEditorOnlyData() : nullptr;
	if (!EditorData || !LeafProperty || !Address)
	{
		return false;
	}
	const uint8* Target = static_cast<const uint8*>(Address);
	for (TFieldIterator<FProperty> It(EditorData->GetClass()); It; ++It)
	{
		const FStructProperty* Field = CastField<FStructProperty>(*It);
		if (!Field || Field->ArrayDim != 1)
		{
			continue;
		}
		void* InputAddress = Field->ContainerPtrToValuePtr<void>(EditorData);
		EMaterialProperty Property = MP_MAX;
		for (int32 Index = 0; Index < MP_MAX; ++Index)
		{
			if (Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(Index)) == InputAddress)
			{
				Property = static_cast<EMaterialProperty>(Index);
				break;
			}
		}
		if (Property == MP_MAX)
		{
			continue;
		}
		const uint8* Begin = static_cast<const uint8*>(InputAddress);
		if (Target < Begin || Target >= Begin + Field->Struct->GetPropertiesSize())
		{
			continue;
		}
		OutProperty = Property;
		OutLeaf.Reset();
		// Empty leaf means the input struct itself was the target; the caller refuses that replacement.
		BuildInputLeafPath(Field->Struct, InputAddress, Address, LeafProperty, OutLeaf);
		return true;
	}
	return false;
}

bool IsPropertyValueTypeValid(const FProperty* Property, const TSharedPtr<FJsonValue>& Value)
{
	if (CastField<FBoolProperty>(Property))
	{
		return Value->Type == EJson::Boolean;
	}
	if (const auto* Number = CastField<FNumericProperty>(Property))
	{
		if (Number->IsEnum())
		{
			return Value->Type == EJson::String || Value->Type == EJson::Number;
		}
		double Numeric;
		if (Value->Type != EJson::Number || !Value->TryGetNumber(Numeric) || !FMath::IsFinite(Numeric))
		{
			return false;
		}
		if (CastField<FFloatProperty>(Property))
		{
			return FMath::Abs(Numeric) <= MAX_flt;
		}
		if (Number->IsInteger())
		{
			// The serializer narrows integer storage with a plain cast, so refuse values the native type
			// cannot hold instead of committing a wrapped value.
			if (Numeric != FMath::FloorToDouble(Numeric))
			{
				return false;
			}
			if (CastField<FByteProperty>(Property))
			{
				return Numeric >= 0.0 && Numeric <= 255.0;
			}
			if (CastField<FIntProperty>(Property))
			{
				return Numeric >= -2147483648.0 && Numeric < 2147483648.0;
			}
			if (CastField<FInt64Property>(Property))
			{
				return Numeric >= -9223372036854775808.0 && Numeric < 9223372036854775808.0;
			}
			// int8/int16/uint16/uint32/uint64 have no serializer branch and could only be narrowed.
			return false;
		}
	}
	if (CastField<FStrProperty>(Property) || CastField<FNameProperty>(Property))
	{
		return Value->Type == EJson::String;
	}
	return true;
}
}

UMaterial* FCortexMaterialAssetOps::LoadMaterial(const FString& AssetPath, FCortexCommandResult& OutError)
{
	FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::MaterialNotFound,
			FString::Printf(TEXT("Material not found: %s"), *AssetPath)
		);
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	if (Material == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::MaterialNotFound,
			FString::Printf(TEXT("Material not found: %s"), *AssetPath)
		);
	}
	return Material;
}

UMaterialInstanceConstant* FCortexMaterialAssetOps::LoadInstance(const FString& AssetPath, FCortexCommandResult& OutError)
{
	FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InstanceNotFound,
			FString::Printf(TEXT("Material instance not found: %s"), *AssetPath)
		);
		return nullptr;
	}

	UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *AssetPath);
	if (Instance == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InstanceNotFound,
			FString::Printf(TEXT("Material instance not found: %s"), *AssetPath)
		);
	}
	return Instance;
}

FCortexCommandResult FCortexMaterialAssetOps::ListMaterials(const TSharedPtr<FJsonObject>& Params)
{
	FString Path = TEXT("/Game/");
	bool bRecursive = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("path"), Path);
		Params->TryGetBoolField(TEXT("recursive"), bRecursive);
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (AssetRegistry == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady, TEXT("Asset Registry not available"));
	}

	FARFilter Filter;
	Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	if (!Path.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*Path));
		Filter.bRecursivePaths = bRecursive;
	}

	TArray<FAssetData> AssetDataList;
	AssetRegistry->GetAssets(Filter, AssetDataList);

	TArray<TSharedPtr<FJsonValue>> MaterialsArray;
	for (const FAssetData& AssetData : AssetDataList)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		Entry->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		MaterialsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("materials"), MaterialsArray);
	Data->SetNumberField(TEXT("count"), MaterialsArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialAssetOps::GetMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}
	bool bIncludeInputState = false;
	if (Params->HasField(TEXT("include_input_state")) && !Params->TryGetBoolField(TEXT("include_input_state"), bIncludeInputState))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("include_input_state must be a boolean"));
	}
	TSharedPtr<FJsonObject> SelectedProperties;
	if (Params->HasField(TEXT("properties")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
		if (!Params->TryGetArrayField(TEXT("properties"), Paths))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("properties must be an array of non-empty paths"));
		}
		if (!Paths->IsEmpty())
		{
			SelectedProperties = MakeShared<FJsonObject>();
			for (const auto& PathValue : *Paths)
			{
				FString Path;
				UObject* Owner = nullptr;
				FProperty* Property = nullptr;
				void* Address = nullptr;
				if (!PathValue.IsValid() || !PathValue->TryGetString(Path) || Path.IsEmpty()
					|| !ResolveOwnedMaterialPath(Material, Path, Owner, Property, Address))
				{
					return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Invalid owned material property path: %s"), *Path));
				}
				auto Value = FCortexSerializer::PropertyToJson(Property, Address);
				// The serializer reports unreadable storage as JSON null, which is indistinguishable from a
				// legitimately null object reference. Refuse the unsupported case instead of publishing a
				// fabricated null in a projection the caller treats as complete.
				if (!Value.IsValid() || (Value->IsNull() && !CastField<FObjectProperty>(Property)))
				{
					return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Unsupported material property read: %s"), *Path));
				}
				SelectedProperties->SetField(Path, Value);
			}
		}
	}

	// Material domain
	FString DomainStr;
	switch (Material->MaterialDomain)
	{
	case MD_Surface:        DomainStr = TEXT("Surface"); break;
	case MD_DeferredDecal:  DomainStr = TEXT("DeferredDecal"); break;
	case MD_LightFunction:  DomainStr = TEXT("LightFunction"); break;
	case MD_PostProcess:    DomainStr = TEXT("PostProcess"); break;
	case MD_UI:             DomainStr = TEXT("UI"); break;
	default:                DomainStr = TEXT("Unknown"); break;
	}

	// Blend mode: authored naming shares the complete enum helper with effective_blend_mode.
	const FString BlendStr = CortexMaterialInputUtils::BlendModeName(Material->BlendMode);

	// Shading model
	FString ShadingStr;
	switch (Material->GetShadingModels().GetFirstShadingModel())
	{
	case MSM_Unlit:         ShadingStr = TEXT("Unlit"); break;
	case MSM_DefaultLit:    ShadingStr = TEXT("DefaultLit"); break;
	case MSM_Subsurface:    ShadingStr = TEXT("Subsurface"); break;
	case MSM_ClearCoat:     ShadingStr = TEXT("ClearCoat"); break;
	default:                ShadingStr = TEXT("Other"); break;
	}

	// Count expression nodes
	int32 NodeCount = 0;
	if (Material->GetEditorOnlyData())
	{
		NodeCount = Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Num();
	}

	// Count parameters by type
	TArray<FMaterialParameterInfo> ScalarParams;
	TArray<FGuid> ScalarGuids;
	Material->GetAllScalarParameterInfo(ScalarParams, ScalarGuids);

	TArray<FMaterialParameterInfo> VectorParams;
	TArray<FGuid> VectorGuids;
	Material->GetAllVectorParameterInfo(VectorParams, VectorGuids);

	TArray<FMaterialParameterInfo> TextureParams;
	TArray<FGuid> TextureGuids;
	Material->GetAllTextureParameterInfo(TextureParams, TextureGuids);

	TSharedPtr<FJsonObject> ParamCount = MakeShared<FJsonObject>();
	ParamCount->SetNumberField(TEXT("scalar"), ScalarParams.Num());
	ParamCount->SetNumberField(TEXT("vector"), VectorParams.Num());
	ParamCount->SetNumberField(TEXT("texture"), TextureParams.Num());

	// Count material instances
	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	int32 InstanceCount = 0;
	if (AssetRegistry)
	{
		FARFilter InstanceFilter;
		InstanceFilter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
		InstanceFilter.bRecursiveClasses = true;

		TArray<FAssetData> AllInstances;
		AssetRegistry->GetAssets(InstanceFilter, AllInstances);

		for (const FAssetData& InstanceData : AllInstances)
		{
			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(InstanceData.GetAsset());
			if (Instance && Instance->Parent == Material)
			{
				InstanceCount++;
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> CollectionsArray;
	TArray<TSharedPtr<FJsonValue>> Sm6Warnings;
	CollectReferencedCollections(Material, CollectionsArray, Sm6Warnings);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Material->GetName());
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("material_domain"), DomainStr);
	Data->SetStringField(TEXT("blend_mode"), BlendStr);
	Data->SetStringField(TEXT("effective_blend_mode"), CortexMaterialInputUtils::BlendModeName(Material->GetBlendMode()));
	Data->SetBoolField(TEXT("can_masked_be_assumed_opaque"), !!Material->bCanMaskedBeAssumedOpaque);
	if (SelectedProperties.IsValid())
	{
		Data->SetObjectField(TEXT("properties"), SelectedProperties);
	}
	Data->SetArrayField(TEXT("warnings"), CortexMaterialInputUtils::CollectWarnings(Material));
	if (bIncludeInputState)
	{
		Data->SetArrayField(TEXT("root_inputs"), CortexMaterialInputUtils::SerializeInputs(Material));
	}
	Data->SetStringField(TEXT("shading_model"), ShadingStr);
	Data->SetBoolField(TEXT("two_sided"), Material->IsTwoSided());
	Data->SetBoolField(TEXT("is_masked"), Material->IsMasked());
	Data->SetNumberField(TEXT("node_count"), NodeCount);
	Data->SetObjectField(TEXT("parameter_count"), ParamCount);
	Data->SetNumberField(TEXT("instance_count"), InstanceCount);
	Data->SetArrayField(TEXT("referenced_collections"), CollectionsArray);
	Data->SetArrayField(TEXT("sm6_warnings"), Sm6Warnings);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialAssetOps::CreateMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Name;
	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		&& Params->TryGetStringField(TEXT("name"), Name);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and name")
		);
	}

	// Normalize trailing slash to prevent double-slash in path joining
	while (AssetPath.EndsWith(TEXT("/")))
	{
		AssetPath = AssetPath.Left(AssetPath.Len() - 1);
	}

	const FString FullPath = FString::Printf(TEXT("%s/%s"), *AssetPath, *Name);
	const FString PkgName = FPackageName::ObjectPathToPackageName(FullPath);
	if (FindPackage(nullptr, *PkgName) || FPackageName::DoesPackageExist(PkgName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetAlreadyExists,
			FString::Printf(TEXT("Asset already exists: %s"), *FullPath)
		);
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Create Material %s"), *Name)
	));

	UObject* NewAsset = AssetTools.CreateAsset(Name, AssetPath, UMaterial::StaticClass(), Factory);
	if (NewAsset == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to create material: %s"), *FullPath)
		);
	}

	// Save to disk
	UPackage* Package = NewAsset->GetOutermost();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, NewAsset, *PackageFilename, SaveArgs);

	FCortexEditorUtils::NotifyAssetModified(NewAsset);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), FullPath);
	Data->SetStringField(TEXT("name"), Name);
	Data->SetBoolField(TEXT("created"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialAssetOps::DeleteMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}

	FString MaterialName = Material->GetName();

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Delete Material %s"), *MaterialName)
	));

	// Get package file path before destroying
	UPackage* Package = Material->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	// Use ObjectTools for proper asset deletion (handles references and cleanup)
	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(Material);
	int32 DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);

	if (DeletedCount == 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to delete material: %s (may have references)"), *AssetPath)
		);
	}

	// Delete the .uasset file from disk if it still exists
	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
	{
		IFileManager::Get().Delete(*PackageFilename);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("deleted"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialAssetOps::ListInstances(const TSharedPtr<FJsonObject>& Params)
{
	FString Path = TEXT("/Game/");
	FString ParentMaterialPath;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("path"), Path);
		Params->TryGetStringField(TEXT("parent_material"), ParentMaterialPath);
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (AssetRegistry == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady, TEXT("Asset Registry not available"));
	}

	FARFilter Filter;
	Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	if (!Path.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*Path));
		Filter.bRecursivePaths = true;
	}

	TArray<FAssetData> AssetDataList;
	AssetRegistry->GetAssets(Filter, AssetDataList);

	TArray<TSharedPtr<FJsonValue>> InstancesArray;
	for (const FAssetData& AssetData : AssetDataList)
	{
		// Optional parent filter
		if (!ParentMaterialPath.IsEmpty())
		{
			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(AssetData.GetAsset());
			if (Instance && Instance->Parent)
			{
				if (Instance->Parent->GetPathName() != ParentMaterialPath)
				{
					continue;
				}
			}
			else
			{
				continue;
			}
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		Entry->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		InstancesArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("instances"), InstancesArray);
	Data->SetNumberField(TEXT("count"), InstancesArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialAssetOps::GetInstance(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UMaterialInstanceConstant* Instance = LoadInstance(AssetPath, LoadError);
	if (Instance == nullptr)
	{
		return LoadError;
	}

	FString ParentMaterialPath;
	if (Instance->Parent)
	{
		ParentMaterialPath = Instance->Parent->GetPathName();
	}

	// Build overrides object
	TArray<TSharedPtr<FJsonValue>> ScalarOverrides;
	for (const FScalarParameterValue& Param : Instance->ScalarParameterValues)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		Entry->SetNumberField(TEXT("value"), Param.ParameterValue);
		ScalarOverrides.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TArray<TSharedPtr<FJsonValue>> VectorOverrides;
	for (const FVectorParameterValue& Param : Instance->VectorParameterValues)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		TArray<TSharedPtr<FJsonValue>> Color;
		Color.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.R));
		Color.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.G));
		Color.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.B));
		Color.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.A));
		Entry->SetArrayField(TEXT("value"), Color);
		VectorOverrides.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TArray<TSharedPtr<FJsonValue>> TextureOverrides;
	for (const FTextureParameterValue& Param : Instance->TextureParameterValues)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		if (Param.ParameterValue)
		{
			Entry->SetStringField(TEXT("value"), Param.ParameterValue->GetPathName());
		}
		TextureOverrides.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Overrides = MakeShared<FJsonObject>();
	Overrides->SetArrayField(TEXT("scalar"), ScalarOverrides);
	Overrides->SetArrayField(TEXT("vector"), VectorOverrides);
	Overrides->SetArrayField(TEXT("texture"), TextureOverrides);

	TArray<TSharedPtr<FJsonValue>> CollectionsArray;
	TArray<TSharedPtr<FJsonValue>> Sm6Warnings;
	CollectReferencedCollections(Instance->GetMaterial(), CollectionsArray, Sm6Warnings);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Instance->GetName());
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("parent_material"), ParentMaterialPath);
	Data->SetObjectField(TEXT("overrides"), Overrides);
	Data->SetArrayField(TEXT("referenced_collections"), CollectionsArray);
	Data->SetArrayField(TEXT("sm6_warnings"), Sm6Warnings);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialAssetOps::CreateInstance(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Name;
	FString ParentMaterialPath;
	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		&& Params->TryGetStringField(TEXT("name"), Name);

	if (bHasParams)
	{
		// Accept "parent" as alias for "parent_material"
		if (!Params->TryGetStringField(TEXT("parent_material"), ParentMaterialPath))
		{
			bHasParams = Params->TryGetStringField(TEXT("parent"), ParentMaterialPath);
		}
	}

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, name, and parent_material (or parent)")
		);
	}

	// Normalize trailing slash
	while (AssetPath.EndsWith(TEXT("/")))
	{
		AssetPath = AssetPath.Left(AssetPath.Len() - 1);
	}

	// Validate parent exists
	FCortexCommandResult LoadError;
	UMaterial* ParentMaterial = LoadMaterial(ParentMaterialPath, LoadError);
	if (ParentMaterial == nullptr)
	{
		return LoadError;
	}

	const FString FullPath = FString::Printf(TEXT("%s/%s"), *AssetPath, *Name);
	const FString PkgName = FPackageName::ObjectPathToPackageName(FullPath);
	if (FindPackage(nullptr, *PkgName) || FPackageName::DoesPackageExist(PkgName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetAlreadyExists,
			FString::Printf(TEXT("Asset already exists: %s"), *FullPath)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Create Material Instance %s"), *Name)
	));

	// Create package and instance
	UPackage* Package = CreatePackage(*PkgName);
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(
		Package, FName(*Name), RF_Public | RF_Standalone);

	Instance->Parent = ParentMaterial;
	Instance->MarkPackageDirty();

	// Save to disk
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, Instance, *PackageFilename, SaveArgs);

	FCortexEditorUtils::NotifyAssetModified(Instance);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), FullPath);
	Data->SetStringField(TEXT("name"), Name);
	Data->SetStringField(TEXT("parent_material"), ParentMaterialPath);
	Data->SetBoolField(TEXT("created"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialAssetOps::DeleteInstance(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UMaterialInstanceConstant* Instance = LoadInstance(AssetPath, LoadError);
	if (Instance == nullptr)
	{
		return LoadError;
	}

	FString InstanceName = Instance->GetName();

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Delete Material Instance %s"), *InstanceName)
	));

	// Get package file path before destroying
	UPackage* Package = Instance->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	// Use ObjectTools for proper asset deletion
	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(Instance);
	int32 DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);

	if (DeletedCount == 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to delete material instance: %s (may have references)"), *AssetPath)
		);
	}

	// Delete the .uasset file from disk if it still exists
	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
	{
		IFileManager::Get().Delete(*PackageFilename);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("deleted"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialAssetOps::SetMaterialProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, PropertyName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, property_name"));
	}

	TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
	if (!Value.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: value"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}

	auto Invalid = [&](const FString& Reason)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Material property '%s': %s"), *PropertyName, *Reason));
	};
	UObject* Owner = nullptr;
	FProperty* Property = nullptr;
	void* PropertyAddress = nullptr;
	if (!ResolveOwnedMaterialPath(Material, PropertyName, Owner, Property, PropertyAddress))
	{
		return Invalid(TEXT("property not found, null object, array path, or external ownership"));
	}
	// The editor-only data object holds every root input and nested editor property. Its wholesale replacement is
	// already refused on the canonical form by the resolver prefix rule; refuse the same replacement when an owned
	// alias route reaches the actual reflected reference, which would swap all root state or borrow a foreign one.
	if (Property == FindFProperty<FObjectProperty>(UMaterial::StaticClass(), TEXT("EditorOnlyData")))
	{
		return Invalid(TEXT("editor-only data replacement is not permitted; author the properties it contains"));
	}
	EMaterialProperty RootProperty = MP_MAX;
	FString Leaf;
	if (ResolveRootInputLeaf(Material, Property, PropertyAddress, RootProperty, Leaf))
	{
		auto Patch = MakeShared<FJsonObject>();
		Patch->SetStringField(TEXT("asset_path"), AssetPath);
		Patch->SetStringField(TEXT("input"), CortexMaterialInputUtils::InputName(RootProperty));
		FString PatchKey;
		if (Leaf.Equals(TEXT("UseConstant"), ESearchCase::IgnoreCase))
		{
			PatchKey = TEXT("use_constant");
		}
		else if (Leaf.Equals(TEXT("Constant"), ESearchCase::IgnoreCase))
		{
			PatchKey = TEXT("constant");
		}
		else if (Leaf.StartsWith(TEXT("Constant."), ESearchCase::IgnoreCase))
		{
			auto State = CortexMaterialInputUtils::SerializeInput(Material, RootProperty);
			const auto Constant = State->TryGetField(TEXT("constant"));
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Constant.IsValid() || !Constant->TryGetObject(Object))
			{
				return Invalid(TEXT("input has no component constant"));
			}
			const FString Component = Leaf.Mid(9).ToLower();
			if (!(*Object)->HasField(Component))
			{
				return Invalid(TEXT("unknown constant component"));
			}
			(*Object)->SetField(Component, Value);
			Patch->SetField(TEXT("constant"), Constant);
		}
		else
		{
			const TCHAR* NativeNames[] = {TEXT("OutputIndex"), TEXT("Mask"), TEXT("MaskR"), TEXT("MaskG"), TEXT("MaskB"), TEXT("MaskA")};
			const TCHAR* Keys[] = {TEXT("output_index"), TEXT("mask"), TEXT("mask_r"), TEXT("mask_g"), TEXT("mask_b"), TEXT("mask_a")};
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Keys); ++Index)
			{
				if (Leaf.Equals(NativeNames[Index], ESearchCase::IgnoreCase))
				{
					PatchKey = Keys[Index];
					break;
				}
			}
			if (PatchKey.IsEmpty())
			{
				return Invalid(TEXT("root inputs require typed field authoring; expression and whole-input replacement are not permitted"));
			}
		}
		if (!PatchKey.IsEmpty())
		{
			Patch->SetField(PatchKey, Value);
		}
		auto Result = CortexMaterialInputUtils::ApplyPatch(Material, RootProperty, Patch);
		if (!Result.bSuccess)
		{
			return Invalid(Result.ErrorMessage);
		}
		Result.Data->SetStringField(TEXT("property_name"), PropertyName);
		return Result;
	}
	if (!IsPropertyValueTypeValid(Property, Value))
	{
		return Invalid(TEXT("value has the wrong type or is outside native numeric storage"));
	}
	FMemMark Mark(FMemStack::Get());
	void* Staged = FMemStack::Get().Alloc(Property->GetSize(), Property->GetMinAlignment());
	Property->InitializeValue(Staged);
	ON_SCOPE_EXIT
	{
		Property->DestroyValue(Staged);
	};
	Property->CopyCompleteValue(Staged, PropertyAddress);
	TArray<FString> Warnings;
	if (!FCortexSerializer::JsonToProperty(Value, Property, Staged, Owner, Warnings))
	{
		return Invalid(Warnings.IsEmpty() ? TEXT("unsupported value") : FString::Join(Warnings, TEXT("; ")));
	}
	FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Set Material Property")), !FCortexCommandRouter::IsInBatch());
	Material->Modify();
	Owner->SetFlags(RF_Transactional);
	Owner->Modify();
	if (!FCortexCommandRouter::IsInBatch())
	{
		Owner->PreEditChange(Property);
		if (Owner != Material)
		{
			Material->PreEditChange(nullptr);
		}
	}
	Property->CopyCompleteValue(PropertyAddress, Staged);
	if (!FCortexCommandRouter::IsInBatch())
	{
		FPropertyChangedEvent Event(Property);
		Owner->PostEditChangeProperty(Event);
		if (Owner != Material)
		{
			Material->PostEditChange();
		}
	}
	else
	{
		FCortexBatchScope::MarkMaterialDirty(Material);
	}
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("property_name"), PropertyName);
	Data->SetBoolField(TEXT("updated"), true);

	return FCortexCommandRouter::Success(Data);
}
