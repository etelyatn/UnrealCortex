#include "Operations/CortexMaterialAssetOps.h"
#include "CortexMaterialModule.h"
#include "CortexEditorUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
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

	// Blend mode
	FString BlendStr;
	switch (Material->BlendMode)
	{
	case BLEND_Opaque:      BlendStr = TEXT("Opaque"); break;
	case BLEND_Masked:      BlendStr = TEXT("Masked"); break;
	case BLEND_Translucent: BlendStr = TEXT("Translucent"); break;
	case BLEND_Additive:    BlendStr = TEXT("Additive"); break;
	case BLEND_Modulate:    BlendStr = TEXT("Modulate"); break;
	default:                BlendStr = TEXT("Unknown"); break;
	}

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

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Material->GetName());
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("material_domain"), DomainStr);
	Data->SetStringField(TEXT("blend_mode"), BlendStr);
	Data->SetStringField(TEXT("shading_model"), ShadingStr);
	Data->SetBoolField(TEXT("two_sided"), Material->IsTwoSided());
	Data->SetBoolField(TEXT("is_masked"), Material->IsMasked());
	Data->SetNumberField(TEXT("node_count"), NodeCount);
	Data->SetObjectField(TEXT("parameter_count"), ParamCount);

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

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Delete Material %s"), *Material->GetName())
	));

	Material->MarkAsGarbage();
	Material->MarkPackageDirty();

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

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Instance->GetName());
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("parent_material"), ParentMaterialPath);
	Data->SetObjectField(TEXT("overrides"), Overrides);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialAssetOps::CreateInstance(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Name;
	FString ParentMaterialPath;
	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		&& Params->TryGetStringField(TEXT("name"), Name)
		&& Params->TryGetStringField(TEXT("parent_material"), ParentMaterialPath);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, name, and parent_material")
		);
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

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Delete Material Instance %s"), *Instance->GetName())
	));

	Instance->MarkAsGarbage();
	Instance->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("deleted"), true);

	return FCortexCommandRouter::Success(Data);
}
