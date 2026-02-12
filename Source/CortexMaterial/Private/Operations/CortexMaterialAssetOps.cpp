#include "Operations/CortexMaterialAssetOps.h"
#include "CortexMaterialModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"

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
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
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
