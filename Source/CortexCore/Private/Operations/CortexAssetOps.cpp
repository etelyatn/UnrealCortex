#include "Operations/CortexAssetOps.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexCommandRouter.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/SavePackage.h"

namespace
{
	bool IsArrayInput(const TSharedPtr<FJsonObject>& Params)
	{
		return Params.IsValid() && Params->HasTypedField<EJson::Array>(TEXT("asset_path"));
	}
}

FAssetData FCortexAssetOps::ResolveLiteralAssetPath(const FString& AssetPath)
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	if (AssetPath.IsEmpty())
	{
		return FAssetData();
	}

	const int32 LastSlash = AssetPath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	const bool bHasObjectSuffix = LastSlash != INDEX_NONE && AssetPath.Mid(LastSlash + 1).Contains(TEXT("."));

	if (bHasObjectSuffix)
	{
		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		if (AssetData.IsValid())
		{
			return AssetData;
		}
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	TArray<FAssetData> PackageAssets;
	AssetRegistry.GetAssetsByPackageName(FName(*PackageName), PackageAssets);
	if (PackageAssets.Num() > 0)
	{
		return PackageAssets[0];
	}

	TArray<FAssetData> DirectPackageAssets;
	AssetRegistry.GetAssetsByPackageName(FName(*AssetPath), DirectPackageAssets);
	if (DirectPackageAssets.Num() > 0)
	{
		return DirectPackageAssets[0];
	}

	return FAssetData();
}

bool FCortexAssetOps::ResolveGlob(
	const FString& Pattern,
	TArray<FAssetData>& OutAssets,
	FCortexCommandResult& OutError)
{
	if (Pattern.IsEmpty() || !Pattern.Contains(TEXT("*")))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidGlob, TEXT("Invalid glob pattern"));
		return false;
	}

	int32 FirstWildcard = INDEX_NONE;
	Pattern.FindChar(TEXT('*'), FirstWildcard);
	const FString Prefix = Pattern.Left(FirstWildcard);
	const int32 LastSlash = Prefix.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	FString RootPath = LastSlash != INDEX_NONE ? Prefix.Left(LastSlash) : TEXT("/Game");
	if (RootPath.IsEmpty())
	{
		RootPath = TEXT("/Game");
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(FName(*RootPath));

	TArray<FAssetData> Candidates;
	AssetRegistry.GetAssets(Filter, Candidates);

	for (const FAssetData& Candidate : Candidates)
	{
		const FString PackagePath = Candidate.PackageName.ToString();
		const FString ObjectPath = Candidate.GetObjectPathString();
		if (PackagePath.MatchesWildcard(Pattern) || ObjectPath.MatchesWildcard(Pattern))
		{
			OutAssets.Add(Candidate);
		}
	}

	if (OutAssets.Num() == 0)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::NoMatches,
			FString::Printf(TEXT("No assets matched glob: %s"), *Pattern)
		);
		return false;
	}

	return true;
}

bool FCortexAssetOps::ResolveAssetPaths(
	const TSharedPtr<FJsonObject>& Params,
	TArray<FAssetData>& OutAssets,
	FCortexCommandResult& OutError)
{
	if (!Params.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* AssetPathArray = nullptr;
	if (Params->TryGetArrayField(TEXT("asset_path"), AssetPathArray) && AssetPathArray != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& PathValue : *AssetPathArray)
		{
			FString AssetPath;
			if (!PathValue.IsValid() || !PathValue->TryGetString(AssetPath))
			{
				OutAssets.Add(FAssetData());
				continue;
			}

			OutAssets.Add(ResolveLiteralAssetPath(AssetPath));
		}
		return true;
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path (string or array)")
		);
		return false;
	}

	if (AssetPath.Contains(TEXT("*")))
	{
		return ResolveGlob(AssetPath, OutAssets, OutError);
	}

	const FAssetData AssetData = ResolveLiteralAssetPath(AssetPath);
	if (!AssetData.IsValid())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Asset not found: %s"), *AssetPath)
		);
		return false;
	}

	OutAssets.Add(AssetData);
	return true;
}

FString FCortexAssetOps::GetAssetTypeName(const UObject* Asset)
{
	return Asset != nullptr && Asset->GetClass() != nullptr ? Asset->GetClass()->GetName() : TEXT("");
}

FCortexCommandResult FCortexAssetOps::SaveAsset(const TSharedPtr<FJsonObject>& Params)
{
	bool bDryRun = false;
	bool bForce = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
		Params->TryGetBoolField(TEXT("force"), bForce);
	}

	TArray<FAssetData> Assets;
	FCortexCommandResult ResolveError;
	if (!ResolveAssetPaths(Params, Assets, ResolveError))
	{
		return ResolveError;
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	ResultsArray.Reserve(Assets.Num());

	for (const FAssetData& AssetData : Assets)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();

		if (!AssetData.IsValid())
		{
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::AssetNotFound);
			Entry->SetStringField(TEXT("message"), TEXT("Asset not found"));
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		const FString AssetPath = AssetData.GetObjectPathString();
		Entry->SetStringField(TEXT("asset_path"), AssetPath);
		Entry->SetStringField(TEXT("asset_type"), AssetData.AssetClassPath.GetAssetName().ToString());

		UObject* Asset = AssetData.GetAsset();
		if (Asset == nullptr)
		{
			Asset = AssetData.GetSoftObjectPath().TryLoad();
		}
		if (Asset == nullptr)
		{
			Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
		}

		if (Asset != nullptr)
		{
			Entry->SetStringField(TEXT("asset_type"), GetAssetTypeName(Asset));
		}

		UPackage* Package = FindPackage(nullptr, *AssetData.PackageName.ToString());
		if (Package == nullptr)
		{
			Package = LoadPackage(nullptr, *AssetData.PackageName.ToString(), LOAD_None);
		}
		if (Package == nullptr)
		{
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::AssetNotFound);
			Entry->SetStringField(TEXT("message"), FString::Printf(TEXT("Failed to load package: %s"), *AssetPath));
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		const bool bWasDirty = Package != nullptr && Package->IsDirty();
		Entry->SetBoolField(TEXT("was_dirty"), bWasDirty);

		if (bDryRun)
		{
			Entry->SetBoolField(TEXT("can_save"), bForce || bWasDirty);
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		bool bSaved = true;
		if (Package != nullptr && (bForce || bWasDirty))
		{
			const FString PackageFilename = FPackageName::LongPackageNameToFilename(
				Package->GetName(),
				FPackageName::GetAssetPackageExtension()
			);

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;

			bSaved = UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs);
		}

		Entry->SetBoolField(TEXT("saved"), bSaved);
		if (!bSaved)
		{
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::SaveFailed);
			Entry->SetStringField(TEXT("message"), FString::Printf(TEXT("Failed to save: %s"), *AssetPath));
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetNumberField(TEXT("count"), ResultsArray.Num());
	if (bDryRun)
	{
		Data->SetBoolField(TEXT("dry_run"), true);
	}

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexAssetOps::OpenAsset(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	if (GEditor == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable, TEXT("Editor not available"));
	}

	bool bDryRun = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	}

	TArray<FAssetData> Assets;
	FCortexCommandResult ResolveError;
	if (!ResolveAssetPaths(Params, Assets, ResolveError))
	{
		return ResolveError;
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (AssetEditorSubsystem == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable, TEXT("Asset editor subsystem unavailable"));
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	ResultsArray.Reserve(Assets.Num());

	for (const FAssetData& AssetData : Assets)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();

		if (!AssetData.IsValid())
		{
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::AssetNotFound);
			Entry->SetStringField(TEXT("message"), TEXT("Asset not found"));
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		const FString AssetPath = AssetData.GetObjectPathString();
		Entry->SetStringField(TEXT("asset_path"), AssetPath);
		Entry->SetStringField(TEXT("asset_type"), AssetData.AssetClassPath.GetAssetName().ToString());

		UObject* Asset = AssetData.GetAsset();
		if (Asset == nullptr)
		{
			Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
		}
		if (Asset == nullptr)
		{
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::AssetNotFound);
			Entry->SetStringField(TEXT("message"), FString::Printf(TEXT("Failed to load: %s"), *AssetPath));
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		Entry->SetStringField(TEXT("asset_type"), GetAssetTypeName(Asset));
		const bool bWasAlreadyOpen = AssetEditorSubsystem->FindEditorsForAsset(Asset).Num() > 0;
		Entry->SetBoolField(TEXT("was_already_open"), bWasAlreadyOpen);

		if (bDryRun)
		{
			Entry->SetBoolField(TEXT("would_open"), true);
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		AssetEditorSubsystem->OpenEditorForAsset(Asset);
		const bool bOpened = AssetEditorSubsystem->FindEditorsForAsset(Asset).Num() > 0;
		Entry->SetBoolField(TEXT("opened"), bOpened);
		if (!bOpened)
		{
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::InvalidOperation);
			Entry->SetStringField(TEXT("message"), FString::Printf(TEXT("Failed to open: %s"), *AssetPath));
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetNumberField(TEXT("count"), ResultsArray.Num());
	if (bDryRun)
	{
		Data->SetBoolField(TEXT("dry_run"), true);
	}

	return FCortexCommandRouter::Success(Data);
#else
	return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable, TEXT("Editor not available"));
#endif
}

FCortexCommandResult FCortexAssetOps::CloseAsset(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	if (GEditor == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable, TEXT("Editor not available"));
	}

	bool bDryRun = false;
	bool bSaveBeforeClose = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
		Params->TryGetBoolField(TEXT("save"), bSaveBeforeClose);
	}

	TArray<FAssetData> Assets;
	FCortexCommandResult ResolveError;
	if (!ResolveAssetPaths(Params, Assets, ResolveError))
	{
		return ResolveError;
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (AssetEditorSubsystem == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable, TEXT("Asset editor subsystem unavailable"));
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	ResultsArray.Reserve(Assets.Num());

	for (const FAssetData& AssetData : Assets)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();

		if (!AssetData.IsValid())
		{
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::AssetNotFound);
			Entry->SetStringField(TEXT("message"), TEXT("Asset not found"));
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		const FString AssetPath = AssetData.GetObjectPathString();
		Entry->SetStringField(TEXT("asset_path"), AssetPath);
		Entry->SetStringField(TEXT("asset_type"), AssetData.AssetClassPath.GetAssetName().ToString());

		UObject* Asset = AssetData.GetAsset();
		if (Asset == nullptr)
		{
			Asset = AssetData.GetSoftObjectPath().TryLoad();
		}
		if (Asset == nullptr)
		{
			Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
		}
		if (Asset == nullptr)
		{
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::AssetNotFound);
			Entry->SetStringField(TEXT("message"), FString::Printf(TEXT("Failed to load: %s"), *AssetPath));
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		Entry->SetStringField(TEXT("asset_type"), GetAssetTypeName(Asset));
		UPackage* Package = Asset->GetOutermost();
		const bool bWasDirty = Package != nullptr && Package->IsDirty();
		const bool bWasOpen = AssetEditorSubsystem->FindEditorsForAsset(Asset).Num() > 0;
		Entry->SetBoolField(TEXT("was_dirty"), bWasDirty);
		Entry->SetBoolField(TEXT("was_open"), bWasOpen);

		if (bDryRun)
		{
			Entry->SetBoolField(TEXT("would_close"), bWasOpen);
			Entry->SetBoolField(TEXT("would_save"), bSaveBeforeClose && bWasDirty);
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		bool bSaved = false;
		if (bSaveBeforeClose && bWasDirty && Package != nullptr)
		{
			const FString PackageFilename = FPackageName::LongPackageNameToFilename(
				Package->GetName(),
				FPackageName::GetAssetPackageExtension()
			);

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			bSaved = UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs);
		}
		Entry->SetBoolField(TEXT("saved"), bSaved);

		const bool bClosed = bWasOpen ? (AssetEditorSubsystem->CloseAllEditorsForAsset(Asset) > 0) : false;
		Entry->SetBoolField(TEXT("closed"), bClosed);

		ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetNumberField(TEXT("count"), ResultsArray.Num());
	if (bDryRun)
	{
		Data->SetBoolField(TEXT("dry_run"), true);
	}

	return FCortexCommandRouter::Success(Data);
#else
	return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable, TEXT("Editor not available"));
#endif
}

FCortexCommandResult FCortexAssetOps::ReloadAsset(const TSharedPtr<FJsonObject>& Params)
{
	(void)Params;
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("core.reload_asset not yet implemented"));
}
