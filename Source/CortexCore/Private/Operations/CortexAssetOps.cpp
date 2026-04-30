#include "Operations/CortexAssetOps.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexAssetFingerprint.h"
#include "CortexBatchMutation.h"
#include "CortexCommandRouter.h"
#include "CortexLogCapture.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/SavePackage.h"

namespace
{
// When a batch result has exactly one entry, copy its fields to the top-level
// Data object for convenient single-asset access (e.g. data.saved instead of data.results[0].saved).
void PromoteSingleResult(TSharedPtr<FJsonObject>& Data, const TArray<TSharedPtr<FJsonValue>>& ResultsArray)
{
	if (ResultsArray.Num() == 1)
	{
		const TSharedPtr<FJsonObject>& Entry = ResultsArray[0]->AsObject();
		if (Entry.IsValid())
		{
			for (const auto& Pair : Entry->Values)
			{
				// Don't overwrite the batch-level fields
				if (Pair.Key == TEXT("results") || Pair.Key == TEXT("count"))
				{
					continue;
				}
				Data->SetField(Pair.Key, Pair.Value);
			}
		}
	}
}

TSharedPtr<FJsonObject> CopyJsonObject(const TSharedPtr<FJsonObject>& Source)
{
	TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
	if (!Source.IsValid())
	{
		return Copy;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
	{
		Copy->SetField(Pair.Key, Pair.Value);
	}

	return Copy;
}

TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Out.Add(MakeShared<FJsonValueString>(Value));
	}
	return Out;
}

FCortexAssetFingerprint MakeAssetFingerprint(const FAssetData& AssetData, UObject* Asset, UPackage* Package)
{
	(void)AssetData;
	return Asset != nullptr
		? MakeObjectAssetFingerprint(Asset)
		: MakePackageAssetFingerprint(Package);
}

TSharedPtr<FJsonObject> BuildBatchResponseData(const FCortexBatchMutationResult& BatchResult)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), BatchResult.Status);
	Data->SetArrayField(TEXT("written_targets"), ToJsonStringArray(BatchResult.WrittenTargets));
	Data->SetArrayField(TEXT("unwritten_targets"), ToJsonStringArray(BatchResult.UnwrittenTargets));

	TArray<TSharedPtr<FJsonValue>> PerItem;
	PerItem.Reserve(BatchResult.PerItem.Num());
	for (const FCortexBatchMutationItemResult& ItemResult : BatchResult.PerItem)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("target"), ItemResult.Target);
		Entry->SetBoolField(TEXT("success"), ItemResult.Result.bSuccess);
		if (ItemResult.Result.Data.IsValid())
		{
			Entry->SetObjectField(TEXT("data"), ItemResult.Result.Data);
		}
		if (!ItemResult.Result.ErrorCode.IsEmpty())
		{
			Entry->SetStringField(TEXT("error_code"), ItemResult.Result.ErrorCode);
		}
		if (!ItemResult.Result.ErrorMessage.IsEmpty())
		{
			Entry->SetStringField(TEXT("error_message"), ItemResult.Result.ErrorMessage);
		}
		if (ItemResult.Result.ErrorDetails.IsValid())
		{
			Entry->SetObjectField(TEXT("error_details"), ItemResult.Result.ErrorDetails);
		}
		if (ItemResult.Result.Warnings.Num() > 0)
		{
			Entry->SetArrayField(TEXT("warnings"), ToJsonStringArray(ItemResult.Result.Warnings));
		}
		PerItem.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Data->SetArrayField(TEXT("per_item"), PerItem);
	return Data;
}

FCortexCommandResult MakeBatchCommandResult(const FCortexBatchMutationResult& BatchResult)
{
	TSharedPtr<FJsonObject> Data = BuildBatchResponseData(BatchResult);
	if (BatchResult.Status == TEXT("committed"))
	{
		return FCortexCommandRouter::Success(Data);
	}

	return FCortexCommandRouter::Error(BatchResult.ErrorCode, BatchResult.ErrorMessage, Data);
}
}

UObject* FCortexAssetOps::LoadAssetWithFallbacks(const FAssetData& AssetData, const FString& AssetPath)
{
	UObject* Asset = AssetData.GetAsset();
	if (Asset == nullptr)
	{
		Asset = AssetData.GetSoftObjectPath().TryLoad();
	}
	if (Asset == nullptr)
	{
		Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	}
	return Asset;
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
#if WITH_EDITOR
	if (GEditor == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable, TEXT("Editor not available"));
	}

	if (Params.IsValid() && Params->HasField(TEXT("items")))
	{
		FCortexBatchMutationRequest Request;
		FCortexCommandResult ParseError;
		if (!FCortexBatchMutation::ParseRequest(Params, TEXT("asset_path"), Request, ParseError))
		{
			return ParseError;
		}

		auto PreflightSaveAsset = [](const FCortexBatchMutationItem& Item) -> FCortexBatchPreflightResult
		{
			const FAssetData AssetData = FCortexAssetOps::ResolveLiteralAssetPath(Item.Target);
			if (!AssetData.IsValid())
			{
				return FCortexBatchPreflightResult::Error(
					CortexErrorCodes::AssetNotFound,
					FString::Printf(TEXT("Asset not found: %s"), *Item.Target));
			}

			UObject* Asset = FCortexAssetOps::LoadAssetWithFallbacks(AssetData, Item.Target);
			UPackage* Package = FindPackage(nullptr, *AssetData.PackageName.ToString());
			if (Package == nullptr)
			{
				Package = LoadPackage(nullptr, *AssetData.PackageName.ToString(), LOAD_None);
			}
			if (Package == nullptr)
			{
				return FCortexBatchPreflightResult::Error(
					CortexErrorCodes::AssetNotFound,
					FString::Printf(TEXT("Failed to load package: %s"), *Item.Target));
			}

			return FCortexBatchPreflightResult::Success(MakeAssetFingerprint(AssetData, Asset, Package).ToJson());
		};

		auto CommitSaveAsset = [](const FCortexBatchMutationItem& Item) -> FCortexCommandResult
		{
			TSharedPtr<FJsonObject> ItemParams = CopyJsonObject(Item.Params);
			ItemParams->SetStringField(TEXT("asset_path"), Item.Target);
			return FCortexAssetOps::SaveAsset(ItemParams);
		};

		return MakeBatchCommandResult(FCortexBatchMutation::Run(
			Request,
			PreflightSaveAsset,
			CommitSaveAsset));
	}

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

		UObject* Asset = LoadAssetWithFallbacks(AssetData, AssetPath);
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

		const bool bWasDirty = Package->IsDirty();
		Entry->SetBoolField(TEXT("was_dirty"), bWasDirty);

		if (bDryRun)
		{
			Entry->SetBoolField(TEXT("can_save"), bForce || bWasDirty);
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		bool bSaved = true;
		if (bForce || bWasDirty)
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
	PromoteSingleResult(Data, ResultsArray);
	if (bDryRun)
	{
		Data->SetBoolField(TEXT("dry_run"), true);
	}

	return FCortexCommandRouter::Success(Data);
#else
	return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable, TEXT("Editor not available"));
#endif
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

		FCortexLogCapture LogCapture;
		UObject* Asset = LoadAssetWithFallbacks(AssetData, AssetPath);
		if (Asset == nullptr)
		{
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::AssetNotFound);
			Entry->SetStringField(TEXT("message"), FString::Printf(TEXT("Failed to load: %s"), *AssetPath));
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		const TArray<FString> AssetWarnings = LogCapture.GetWarnings(PackageName);
		if (AssetWarnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningsArray;
			WarningsArray.Reserve(AssetWarnings.Num());
			for (const FString& Warning : AssetWarnings)
			{
				WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
			}
			Entry->SetArrayField(TEXT("warnings"), WarningsArray);
		}

		Entry->SetStringField(TEXT("asset_type"), GetAssetTypeName(Asset));

		if (Asset->IsA<UWorld>())
		{
			const FString RequestedPackageName = FPackageName::ObjectPathToPackageName(AssetPath);
			UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
			const bool bIsAlreadyOpen = EditorWorld != nullptr
				&& EditorWorld->GetOutermost()->GetName() == RequestedPackageName;

			Entry->SetBoolField(TEXT("was_already_open"), bIsAlreadyOpen);

			if (bDryRun)
			{
				Entry->SetBoolField(TEXT("would_open"), true);
				ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}

			if (bIsAlreadyOpen)
			{
				Entry->SetBoolField(TEXT("editor_opened"), true);
				ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}

			if (GEditor->PlayWorld)
			{
				GEditor->EndPlayMap();
			}

			FString MapFilePath;
			if (!FPackageName::TryConvertLongPackageNameToFilename(
					RequestedPackageName, MapFilePath, FPackageName::GetMapPackageExtension()))
			{
				Entry->SetStringField(TEXT("error"), CortexErrorCodes::InvalidOperation);
				Entry->SetStringField(TEXT("message"),
					FString::Printf(TEXT("Failed to resolve map path: %s"), *AssetPath));
				ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}

			UWorld* LoadedWorld = UEditorLoadingAndSavingUtils::LoadMap(MapFilePath);
			const bool bOpened = LoadedWorld != nullptr;
			Entry->SetBoolField(TEXT("editor_opened"), bOpened);
			if (!bOpened)
			{
				Entry->SetStringField(TEXT("error"), CortexErrorCodes::InvalidOperation);
				Entry->SetStringField(TEXT("message"),
					FString::Printf(TEXT("Failed to load map: %s"), *AssetPath));
			}

			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

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
		Entry->SetBoolField(TEXT("editor_opened"), bOpened);
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
	PromoteSingleResult(Data, ResultsArray);
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

		UObject* Asset = LoadAssetWithFallbacks(AssetData, AssetPath);
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
	PromoteSingleResult(Data, ResultsArray);
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

	struct FReloadInfo
	{
		FString AssetPath;
		FString AssetType;
		bool bWasDirty = false;
		bool bWasOpen = false;
		bool bHasDiskFile = false;
		UPackage* Package = nullptr;
	};

	TArray<FReloadInfo> ReloadInfos;
	TArray<UPackage*> PackagesToReload;
	ReloadInfos.Reserve(Assets.Num());

	for (const FAssetData& AssetData : Assets)
	{
		FReloadInfo Info;

		if (!AssetData.IsValid())
		{
			ReloadInfos.Add(Info);
			continue;
		}

		Info.AssetPath = AssetData.GetObjectPathString();
		Info.AssetType = AssetData.AssetClassPath.GetAssetName().ToString();

		UObject* Asset = LoadAssetWithFallbacks(AssetData, Info.AssetPath);
		if (Asset == nullptr)
		{
			ReloadInfos.Add(Info);
			continue;
		}

		Info.AssetType = GetAssetTypeName(Asset);
		Info.Package = Asset->GetOutermost();
		Info.bWasDirty = Info.Package != nullptr && Info.Package->IsDirty();
		Info.bWasOpen = AssetEditorSubsystem->FindEditorsForAsset(Asset).Num() > 0;
		Info.bHasDiskFile = Info.Package != nullptr && FPackageName::DoesPackageExist(Info.Package->GetName());

		if (Info.bHasDiskFile && !bDryRun && Info.Package != nullptr)
		{
			if (Info.bWasOpen)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(Asset);
			}
			PackagesToReload.AddUnique(Info.Package);
		}

		ReloadInfos.Add(Info);
	}

	// ReloadPackages returns a single bool for the entire batch — if one package
	// fails, all are reported as failed. Individual failure tracking would require
	// reloading packages one at a time.
	bool bReloadSucceeded = true;
	if (!bDryRun && PackagesToReload.Num() > 0)
	{
		FText ReloadError;
		bReloadSucceeded = UPackageTools::ReloadPackages(
			PackagesToReload,
			ReloadError,
			EReloadPackagesInteractionMode::AssumePositive
		);
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	ResultsArray.Reserve(ReloadInfos.Num());

	for (const FReloadInfo& Info : ReloadInfos)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Info.AssetPath);

		if (Info.AssetPath.IsEmpty() || Info.AssetType.IsEmpty())
		{
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::AssetNotFound);
			Entry->SetStringField(TEXT("message"), TEXT("Asset not found"));
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		Entry->SetStringField(TEXT("asset_type"), Info.AssetType);
		Entry->SetBoolField(TEXT("was_dirty"), Info.bWasDirty);

		if (!Info.bHasDiskFile)
		{
			Entry->SetBoolField(TEXT("reloaded"), false);
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::NoDiskFile);
			Entry->SetStringField(TEXT("message"), TEXT("Asset has no saved package on disk"));
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		if (bDryRun)
		{
			Entry->SetBoolField(TEXT("has_disk_file"), true);
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		const bool bFailed = Info.Package == nullptr || !bReloadSucceeded;
		Entry->SetBoolField(TEXT("reloaded"), !bFailed);
		Entry->SetBoolField(TEXT("discarded_changes"), Info.bWasDirty && !bFailed);

		if (!bFailed && Info.bWasOpen)
		{
			UObject* ReloadedAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *Info.AssetPath);
			if (ReloadedAsset != nullptr)
			{
				AssetEditorSubsystem->OpenEditorForAsset(ReloadedAsset);
			}
		}

		if (bFailed)
		{
			Entry->SetStringField(TEXT("error"), CortexErrorCodes::InvalidOperation);
			Entry->SetStringField(TEXT("message"), FString::Printf(TEXT("Failed to reload: %s"), *Info.AssetPath));
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetNumberField(TEXT("count"), ResultsArray.Num());
	PromoteSingleResult(Data, ResultsArray);
	if (bDryRun)
	{
		Data->SetBoolField(TEXT("dry_run"), true);
	}

	return FCortexCommandRouter::Success(Data);
#else
	return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable, TEXT("Editor not available"));
#endif
}
