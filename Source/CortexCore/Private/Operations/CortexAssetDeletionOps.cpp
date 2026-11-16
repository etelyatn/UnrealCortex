#include "Operations/CortexAssetDeletionOps.h"
#include "CortexCommandRouter.h"
#include "CortexCoreModule.h"
#include "CortexEditorUtils.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
TSharedPtr<FJsonObject> MakeReferencerDetails(const TArray<FString>& Referencers)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Referencers.Num());
	for (const FString& Referencer : Referencers)
	{
		Values.Add(MakeShared<FJsonValueString>(Referencer));
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("referencers"), Values);
	return Details;
}

TSharedPtr<FJsonObject> MakeAssetEntry(const FString& AssetPath, const bool bDeleted, const FString& Error)
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("asset_path"), AssetPath);
	Entry->SetBoolField(TEXT("deleted"), bDeleted);
	if (!Error.IsEmpty())
	{
		Entry->SetStringField(TEXT("error"), Error);
	}

	return Entry;
}

/** True when the folder path names a mounted content root itself (for example /Game) rather than a
 *  folder inside one. Deleting a whole content root is never a single-asset operation. */
bool IsContentRootFolder(const FString& NormalizedFolderPath)
{
	FString RemainingPath = NormalizedFolderPath;
	RemainingPath.RemoveFromStart(TEXT("/"));

	TArray<FString> Segments;
	RemainingPath.ParseIntoArray(Segments, TEXT("/"), true);
	return Segments.Num() < 2;
}
}

FCortexCommandResult FCortexAssetDeletionOps::DeleteAsset(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
	}

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);

	FString MountError;
	if (!FCortexEditorUtils::IsWritableMountedContentPath(PackageName, MountError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, MountError);
	}

	if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	const FString ObjectPath = FString::Printf(
		TEXT("%s.%s"),
		*PackageName,
		*FPackageName::GetShortName(PackageName));

	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
	if (Asset == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
	}

	TArray<FString> Referencers;
	if (!bForce)
	{
		TArray<FName> PackageNames;
		PackageNames.Add(FName(*PackageName));
		Referencers = FCortexEditorUtils::GetExternalReferencers(PackageNames);
	}

	if (Referencers.Num() > 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::HasReferences,
			FString::Printf(
				TEXT("Asset '%s' is referenced by %d package(s). Use force=true to delete anyway."),
				*AssetPath,
				Referencers.Num()),
			MakeReferencerDetails(Referencers));
	}

	TArray<UObject*> AssetsToDelete;
	AssetsToDelete.Add(Asset);
	const TArray<FString> DeleteErrors = FCortexEditorUtils::DeleteLoadedAssets(AssetsToDelete);
	if (!DeleteErrors[0].IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("%s (asset_path: %s)"), *DeleteErrors[0], *AssetPath),
			MakeReferencerDetails(Referencers));
	}

	UE_LOG(LogCortex, Log, TEXT("Deleted asset: %s"), *AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("deleted"), true);
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexAssetDeletionOps::DeleteFolder(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
	}

	FString FolderPath;
	if (!Params->TryGetStringField(TEXT("folder_path"), FolderPath) || FolderPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'folder_path' field"));
	}

	bool bRecursive = true;
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	const FString NormalizedFolderPath = FCortexEditorUtils::NormalizeMountedContentPath(FolderPath);

	FString MountError;
	if (!FCortexEditorUtils::IsWritableMountedContentPath(NormalizedFolderPath, MountError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, MountError);
	}

	if (IsContentRootFolder(NormalizedFolderPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(
				TEXT("Refusing to delete content root '%s': pass a folder inside the root."),
				*NormalizedFolderPath));
	}

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> AssetsInFolder;
	AssetRegistry.GetAssetsByPath(FName(*NormalizedFolderPath), AssetsInFolder, bRecursive);

	if (AssetsInFolder.Num() == 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("deleted_count"), 0);
		Data->SetStringField(TEXT("folder_path"), NormalizedFolderPath);
		Data->SetArrayField(TEXT("assets"), TArray<TSharedPtr<FJsonValue>>());
		return FCortexCommandRouter::Success(Data);
	}

	TArray<FString> Referencers;
	if (!bForce)
	{
		TArray<FName> PackageNames;
		PackageNames.Reserve(AssetsInFolder.Num());
		for (const FAssetData& AssetData : AssetsInFolder)
		{
			PackageNames.Add(AssetData.PackageName);
		}

		Referencers = FCortexEditorUtils::GetExternalReferencers(PackageNames);
	}

	if (Referencers.Num() > 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::HasReferences,
			FString::Printf(
				TEXT("Assets in folder '%s' are referenced by %d package(s) outside it. Use force=true to delete anyway."),
				*NormalizedFolderPath,
				Referencers.Num()),
			MakeReferencerDetails(Referencers));
	}

	TArray<UObject*> LoadedAssets;
	TArray<FString> LoadedAssetPaths;
	TArray<TSharedPtr<FJsonValue>> FailedAssets;

	for (const FAssetData& AssetData : AssetsInFolder)
	{
		const FString AssetPath = AssetData.GetObjectPathString();
		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
		if (Asset == nullptr)
		{
			FailedAssets.Add(MakeShared<FJsonValueObject>(MakeAssetEntry(
				AssetPath,
				false,
				FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath))));
			continue;
		}

		LoadedAssets.Add(Asset);
		LoadedAssetPaths.Add(AssetPath);
	}

	const TArray<FString> DeleteErrors = FCortexEditorUtils::DeleteLoadedAssets(LoadedAssets);

	TArray<TSharedPtr<FJsonValue>> DeletedAssets;
	int32 DeletedCount = 0;

	for (int32 Index = 0; Index < LoadedAssets.Num(); ++Index)
	{
		if (DeleteErrors[Index].IsEmpty())
		{
			DeletedAssets.Add(MakeShared<FJsonValueObject>(MakeAssetEntry(LoadedAssetPaths[Index], true, FString())));
			++DeletedCount;
		}
		else
		{
			FailedAssets.Add(MakeShared<FJsonValueObject>(MakeAssetEntry(LoadedAssetPaths[Index], false, DeleteErrors[Index])));
		}
	}

	UE_LOG(LogCortex, Log, TEXT("Deleted %d assets from folder: %s"), DeletedCount, *NormalizedFolderPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("deleted_count"), DeletedCount);
	Data->SetStringField(TEXT("folder_path"), NormalizedFolderPath);
	Data->SetArrayField(TEXT("assets"), DeletedAssets);
	if (FailedAssets.Num() > 0)
	{
		Data->SetArrayField(TEXT("failed"), FailedAssets);
	}
	return FCortexCommandRouter::Success(Data);
}
