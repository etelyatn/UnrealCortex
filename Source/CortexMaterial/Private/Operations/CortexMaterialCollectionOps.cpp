#include "Operations/CortexMaterialCollectionOps.h"
#include "CortexMaterialModule.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"

UMaterialParameterCollection* FCortexMaterialCollectionOps::LoadCollection(const FString& AssetPath, FCortexCommandResult& OutError)
{
	FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::CollectionNotFound,
			FString::Printf(TEXT("Material parameter collection not found: %s"), *AssetPath)
		);
		return nullptr;
	}

	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr, *AssetPath);
	if (Collection == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::CollectionNotFound,
			FString::Printf(TEXT("Material parameter collection not found: %s"), *AssetPath)
		);
	}
	return Collection;
}

FCortexCommandResult FCortexMaterialCollectionOps::ListCollections(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialCollectionOps::GetCollection(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialCollectionOps::CreateCollection(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialCollectionOps::DeleteCollection(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialCollectionOps::AddCollectionParameter(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialCollectionOps::RemoveCollectionParameter(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialCollectionOps::SetCollectionParameter(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}
