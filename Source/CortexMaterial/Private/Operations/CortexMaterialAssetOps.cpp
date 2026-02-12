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
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialAssetOps::GetMaterial(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
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
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialAssetOps::ListInstances(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialAssetOps::GetInstance(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialAssetOps::CreateInstance(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialAssetOps::DeleteInstance(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}
