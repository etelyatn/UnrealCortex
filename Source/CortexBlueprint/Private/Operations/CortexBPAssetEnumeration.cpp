#include "Operations/CortexBPAssetEnumeration.h"

#include "Algo/Sort.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Operations/CortexBPAssetOps.h"

namespace CortexBPAssetEnumerationPrivate
{
	bool IsPlainFolderPath(const FString& Path)
	{
		return Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT("..")) && !Path.Contains(TEXT("//")) && !Path.Contains(TEXT("."));
	}
}

bool FCortexBPAssetEnumeration::ParseRequest(
	const TSharedPtr<FJsonObject>& Params, FCortexBPEnumerationRequest& OutRequest, FCortexCommandResult& OutError)
{
	using namespace CortexBPAssetEnumerationPrivate;

	OutRequest = FCortexBPEnumerationRequest();
	if (!Params.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
		return false;
	}

	const bool bHasPrefix = Params->HasField(TEXT("path_prefix"));
	const bool bHasPaths = Params->HasField(TEXT("paths"));
	if (bHasPrefix == bHasPaths)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("Provide exactly one of path_prefix (folder) or paths (array of asset paths)"));
		return false;
	}

	OutRequest.bHasPrefix = bHasPrefix;
	if (bHasPrefix)
	{
		FString Prefix;
		if (!Params->TryGetStringField(TEXT("path_prefix"), Prefix) || Prefix.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("path_prefix must be a non-empty folder path"));
			return false;
		}
		Prefix = FCortexEditorUtils::NormalizeMountedContentPath(Prefix);
		while (Prefix.Len() > 1 && Prefix.EndsWith(TEXT("/")))
		{
			Prefix.LeftChopInline(1);
		}
		if (!IsPlainFolderPath(Prefix))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("path_prefix is not a valid mounted folder path: %s"), *Prefix));
			return false;
		}
		OutRequest.Prefix = MoveTemp(Prefix);
	}
	else
	{
		const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
		if (!Params->TryGetArrayField(TEXT("paths"), PathValues) || PathValues->Num() == 0)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("paths must be a non-empty array of asset paths"));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *PathValues)
		{
			FString Path;
			if (!Value.IsValid() || !Value->TryGetString(Path) || Path.IsEmpty())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("paths must be an array of non-empty asset path strings"));
				return false;
			}
			OutRequest.RequestedPaths.Add(Path);
		}
	}

	const TArray<FString>& KnownTypes = FCortexBPAssetOps::GetBlueprintTypeNames();
	OutRequest.bTypeFilter = Params->HasField(TEXT("types"));
	if (OutRequest.bTypeFilter)
	{
		const TArray<TSharedPtr<FJsonValue>>* TypeValues = nullptr;
		if (!Params->TryGetArrayField(TEXT("types"), TypeValues))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("types must be an array of Blueprint types"));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *TypeValues)
		{
			FString Requested;
			const FString* Canonical = nullptr;
			if (Value.IsValid() && Value->TryGetString(Requested))
			{
				Canonical = KnownTypes.FindByPredicate([&Requested](const FString& Known) { return Known.Equals(Requested, ESearchCase::IgnoreCase); });
			}
			if (!Canonical)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Unknown Blueprint type '%s' (valid: %s)"), *Requested, *FString::Join(KnownTypes, TEXT(", "))));
				return false;
			}
			OutRequest.Types.Add(*Canonical);
		}
	}

	FString OutPath;
	if (!Params->TryGetStringField(TEXT("out_path"), OutPath) || OutPath.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: out_path"));
		return false;
	}
	// An unusable out_path is refused before any Blueprint is touched.
	FString PathErrorCode;
	FString PathErrorMessage;
	if (!FCortexSafeFileContract::ResolveWritePath(OutPath, OutRequest.OutPath, PathErrorCode, PathErrorMessage))
	{
		OutError = FCortexCommandRouter::Error(PathErrorCode, PathErrorMessage);
		return false;
	}
	return true;
}

FCortexBPEnumeration FCortexBPAssetEnumeration::Enumerate(const FCortexBPEnumerationRequest& Request)
{
	FCortexBPEnumeration Result;

	// Enumerate through the Asset Registry; nothing is loaded here.
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FTopLevelAssetPath BlueprintClassPath = UBlueprint::StaticClass()->GetClassPathName();
	const FTopLevelAssetPath LevelScriptClassPath = ULevelScriptBlueprint::StaticClass()->GetClassPathName();

	TMap<FString, FAssetData> Found;
	const auto Collect = [&](const FARFilter& Filter)
	{
		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath != LevelScriptClassPath)
			{
				Found.Add(Asset.GetObjectPathString(), Asset);
			}
		}
		return Assets.Num() > 0;
	};

	if (Request.bHasPrefix)
	{
		Registry.ScanPathsSynchronous({Request.Prefix}, true);
		FARFilter Filter;
		Filter.ClassPaths.Add(BlueprintClassPath);
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(FName(*Request.Prefix));
		Filter.bRecursivePaths = true;
		Collect(Filter);
	}
	else
	{
		for (const FString& Requested : Request.RequestedPaths)
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(FCortexEditorUtils::NormalizeMountedContentPath(Requested));
			FARFilter Filter;
			Filter.ClassPaths.Add(BlueprintClassPath);
			Filter.bRecursiveClasses = true;
			Filter.PackageNames.Add(FName(*PackageName));
			if (!Collect(Filter))
			{
				// The package may simply not be scanned yet.
				Registry.ScanPathsSynchronous({FPackageName::GetLongPackagePath(PackageName)}, false);
				if (!Collect(Filter))
				{
					Result.NotFound.Add(Requested);
				}
			}
		}
	}

	if (Request.bHasPrefix)
	{
		Result.MatchedAssets = Found.Num();
	}

	for (const TPair<FString, FAssetData>& Pair : Found)
	{
		const FString Type = FCortexBPAssetOps::DetermineBlueprintTypeFromAssetData(Pair.Value);
		if (Request.bTypeFilter && !Request.Types.Contains(Type))
		{
			++Result.ExcludedByType;
			continue;
		}
		Result.Assets.Add({Pair.Value, Type});
	}
	Algo::SortBy(Result.Assets, [](const FCortexBPEnumeratedAsset& Entry) { return Entry.Asset.GetObjectPathString(); },
		[](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	return Result;
}

void FCortexBPAssetEnumeration::WriteRequestFields(const FCortexBPEnumerationRequest& Request, FJsonObject& Report)
{
	if (Request.bHasPrefix)
	{
		Report.SetStringField(TEXT("path_prefix"), Request.Prefix);
	}
	else
	{
		TArray<TSharedPtr<FJsonValue>> PathValues;
		for (const FString& Requested : Request.RequestedPaths)
		{
			PathValues.Add(MakeShared<FJsonValueString>(Requested));
		}
		Report.SetArrayField(TEXT("paths"), PathValues);
	}
	if (Request.bTypeFilter)
	{
		TArray<TSharedPtr<FJsonValue>> TypeValues;
		for (const FString& Type : FCortexBPAssetOps::GetBlueprintTypeNames())
		{
			if (Request.Types.Contains(Type))
			{
				TypeValues.Add(MakeShared<FJsonValueString>(Type));
			}
		}
		Report.SetArrayField(TEXT("types"), TypeValues);
	}
}
