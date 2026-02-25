#include "Operations/CortexAssetDeletionOps.h"
#include "CortexCommandRouter.h"
#include "CortexCoreModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Misc/TextBuffer.h"
#include "UObject/SavePackage.h"

namespace
{
	bool TryDeletePackageFile(const FString& PackageFilename, int32 MaxAttempts)
	{
		for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
		{
			if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
			{
				return true;
			}
			if (IFileManager::Get().Delete(*PackageFilename, false, false, true))
			{
				return true;
			}
			FPlatformProcess::Sleep(0.01f);
		}
		return !FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename);
	}

	bool DeleteSingleAsset(const FString& AssetPath, FString& OutError)
	{
		const FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
		if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
		{
			OutError = FString::Printf(TEXT("Asset not found: %s"), *AssetPath);
			return false;
		}

		const FString ObjName = FPackageName::GetShortName(PkgName);
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PkgName, *ObjName);

		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath);
			return false;
		}

		UPackage* Package = Asset->GetOutermost();
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(),
			FPackageName::GetAssetPackageExtension());

		// Delete on-disk package first to avoid file watcher race warnings.
		if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
		{
			TryDeletePackageFile(PackageFilename, 20);
		}

		// Guard object prevents false-positive "package marked deleted but modified on disk" warnings
		// if a queued file watcher event races with ForceDeleteObjects.
		UTextBuffer* DeleteGuard = nullptr;
		if (IsValid(Package))
		{
			DeleteGuard = NewObject<UTextBuffer>(Package, TEXT("__CortexDeleteGuard__"), RF_Public);
		}

		TArray<UObject*> ObjectsToDelete;
		ObjectsToDelete.Add(Asset);
		const int32 DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);

		if (DeleteGuard != nullptr)
		{
			DeleteGuard->ClearFlags(RF_Public | RF_Standalone);
		}

		// Retry file deletion after object destruction if file still exists on disk.
		if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
		{
			TryDeletePackageFile(PackageFilename, 20);
		}

		if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
		{
			OutError = FString::Printf(TEXT("Failed to delete asset file: %s"), *PackageFilename);
			return false;
		}

		if (DeletedCount == 0)
		{
			OutError = FString::Printf(TEXT("Failed to delete asset: %s (may have references)"), *AssetPath);
			return false;
		}

		return true;
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

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Delete Asset %s"), *AssetPath)));

	FString Error;
	if (!DeleteSingleAsset(AssetPath, Error))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound, Error);
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

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> AssetsInFolder;
	AssetRegistry.GetAssetsByPath(FName(*FolderPath), AssetsInFolder, bRecursive);

	if (AssetsInFolder.Num() == 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("deleted_count"), 0);
		Data->SetStringField(TEXT("folder_path"), FolderPath);
		Data->SetArrayField(TEXT("assets"), TArray<TSharedPtr<FJsonValue>>());
		return FCortexCommandRouter::Success(Data);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Delete Folder %s"), *FolderPath)));

	TArray<TSharedPtr<FJsonValue>> DeletedAssets;
	TArray<TSharedPtr<FJsonValue>> FailedAssets;
	int32 DeletedCount = 0;

	for (const FAssetData& AssetData : AssetsInFolder)
	{
		const FString AssetPath = AssetData.GetObjectPathString();
		FString Error;
		if (DeleteSingleAsset(AssetPath, Error))
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), AssetPath);
			Entry->SetBoolField(TEXT("deleted"), true);
			DeletedAssets.Add(MakeShared<FJsonValueObject>(Entry));
			++DeletedCount;
		}
		else
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), AssetPath);
			Entry->SetBoolField(TEXT("deleted"), false);
			Entry->SetStringField(TEXT("error"), Error);
			FailedAssets.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	UE_LOG(LogCortex, Log, TEXT("Deleted %d assets from folder: %s"), DeletedCount, *FolderPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("deleted_count"), DeletedCount);
	Data->SetStringField(TEXT("folder_path"), FolderPath);
	Data->SetArrayField(TEXT("assets"), DeletedAssets);
	if (FailedAssets.Num() > 0)
	{
		Data->SetArrayField(TEXT("failed"), FailedAssets);
	}
	return FCortexCommandRouter::Success(Data);
}
