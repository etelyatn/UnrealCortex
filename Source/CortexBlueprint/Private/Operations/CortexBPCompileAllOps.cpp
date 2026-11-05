#include "Operations/CortexBPCompileAllOps.h"

#include "Algo/Sort.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexCommandRouter.h"
#include "CortexEditorUtils.h"
#include "CortexSafeFileContract.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Operations/CortexBPAssetOps.h"

namespace CortexBPCompileAllPrivate
{
	/** Failing assets echoed in the response; the full list is in the report file. */
	constexpr int32 MaxFirstFailures = 10;

	const TArray<FString>& KnownTypes()
	{
		static const TArray<FString> Types = {
			TEXT("Actor"), TEXT("Component"), TEXT("Widget"), TEXT("Interface"), TEXT("FunctionLibrary"), TEXT("Unknown")};
		return Types;
	}

	/** Order matches the totals counters. */
	enum class EStatus : uint8 { Success, Warning, Error, Skipped, NotFound };

	const TCHAR* StatusName(const EStatus Status)
	{
		switch (Status)
		{
		case EStatus::Success: return TEXT("success");
		case EStatus::Warning: return TEXT("warning");
		case EStatus::Error: return TEXT("error");
		case EStatus::Skipped: return TEXT("skipped");
		default: return TEXT("not_found");
		}
	}

	struct FEntry
	{
		FString AssetPath;
		FString Type;
		EStatus Status = EStatus::Error;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		FString Reason;
	};

	TSharedPtr<FJsonObject> ToJson(const FEntry& Entry, const bool bWithDiagnostics)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("asset_path"), Entry.AssetPath);
		Json->SetStringField(TEXT("type"), Entry.Type);
		Json->SetStringField(TEXT("compile_status"), StatusName(Entry.Status));
		Json->SetNumberField(TEXT("error_count"), Entry.ErrorCount);
		Json->SetNumberField(TEXT("warning_count"), Entry.WarningCount);
		if (bWithDiagnostics)
		{
			Json->SetArrayField(TEXT("diagnostics"), Entry.Diagnostics);
		}
		if (!Entry.Reason.IsEmpty())
		{
			Json->SetStringField(TEXT("reason"), Entry.Reason);
		}
		return Json;
	}

	FEntry CompileOne(const FAssetData& Asset, const FString& Type)
	{
		FEntry Entry;
		Entry.AssetPath = Asset.GetObjectPathString();
		Entry.Type = Type;

		FString WritableError;
		if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(Entry.AssetPath, WritableError))
		{
			Entry.Status = EStatus::Skipped;
			Entry.Reason = WritableError;
			return Entry;
		}

		TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
		CompileParams->SetStringField(TEXT("asset_path"), Entry.AssetPath);
		const FCortexCommandResult Result = FCortexBPAssetOps::Compile(CompileParams);

		// Compile answers a failed build with COMPILE_FAILED and the payload in the error details.
		const TSharedPtr<FJsonObject> Payload = Result.bSuccess ? Result.Data : Result.ErrorDetails;
		FString CompileStatus;
		if (!Payload.IsValid() || !Payload->TryGetStringField(TEXT("compile_status"), CompileStatus))
		{
			Entry.Status = EStatus::Error;
			Entry.Reason = Result.ErrorMessage;
			return Entry;
		}
		Entry.Status = CompileStatus == TEXT("error") ? EStatus::Error
			: CompileStatus == TEXT("warning") ? EStatus::Warning : EStatus::Success;
		Entry.ErrorCount = static_cast<int32>(Payload->GetNumberField(TEXT("error_count")));
		Entry.WarningCount = static_cast<int32>(Payload->GetNumberField(TEXT("warning_count")));
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		if (Payload->TryGetArrayField(TEXT("diagnostics"), Diagnostics))
		{
			Entry.Diagnostics = *Diagnostics;
		}
		return Entry;
	}

	bool IsPlainFolderPath(const FString& Path)
	{
		return Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT("..")) && !Path.Contains(TEXT("//")) && !Path.Contains(TEXT("."));
	}
}

FCortexCommandResult FCortexBPCompileAllOps::CompileAll(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexBPCompileAllPrivate;

	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	const bool bHasPrefix = Params->HasField(TEXT("path_prefix"));
	const bool bHasPaths = Params->HasField(TEXT("paths"));
	if (bHasPrefix == bHasPaths)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("Provide exactly one of path_prefix (folder) or paths (array of asset paths)"));
	}

	FString Prefix;
	TArray<FString> RequestedPaths;
	if (bHasPrefix)
	{
		if (!Params->TryGetStringField(TEXT("path_prefix"), Prefix) || Prefix.IsEmpty())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("path_prefix must be a non-empty folder path"));
		}
		Prefix = FCortexEditorUtils::NormalizeMountedContentPath(Prefix);
		while (Prefix.Len() > 1 && Prefix.EndsWith(TEXT("/")))
		{
			Prefix.LeftChopInline(1);
		}
		if (!IsPlainFolderPath(Prefix))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("path_prefix is not a valid mounted folder path: %s"), *Prefix));
		}
	}
	else
	{
		const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
		if (!Params->TryGetArrayField(TEXT("paths"), PathValues) || PathValues->Num() == 0)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("paths must be a non-empty array of asset paths"));
		}
		for (const TSharedPtr<FJsonValue>& Value : *PathValues)
		{
			FString Path;
			if (!Value.IsValid() || !Value->TryGetString(Path) || Path.IsEmpty())
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("paths must be an array of non-empty asset path strings"));
			}
			RequestedPaths.Add(Path);
		}
	}

	TSet<FString> Types;
	const bool bTypeFilter = Params->HasField(TEXT("types"));
	if (bTypeFilter)
	{
		const TArray<TSharedPtr<FJsonValue>>* TypeValues = nullptr;
		if (!Params->TryGetArrayField(TEXT("types"), TypeValues))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("types must be an array of Blueprint types"));
		}
		for (const TSharedPtr<FJsonValue>& Value : *TypeValues)
		{
			FString Requested;
			const FString* Canonical = nullptr;
			if (Value.IsValid() && Value->TryGetString(Requested))
			{
				Canonical = KnownTypes().FindByPredicate([&Requested](const FString& Known) { return Known.Equals(Requested, ESearchCase::IgnoreCase); });
			}
			if (!Canonical)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Unknown Blueprint type '%s' (valid: %s)"), *Requested, *FString::Join(KnownTypes(), TEXT(", "))));
			}
			Types.Add(*Canonical);
		}
	}

	FString OutPath;
	if (!Params->TryGetStringField(TEXT("out_path"), OutPath) || OutPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: out_path"));
	}
	// An unusable out_path is refused before any Blueprint is touched.
	FCortexResolvedFilePath ResolvedOutPath;
	FString PathErrorCode;
	FString PathErrorMessage;
	if (!FCortexSafeFileContract::ResolveWritePath(OutPath, ResolvedOutPath, PathErrorCode, PathErrorMessage))
	{
		return FCortexCommandRouter::Error(PathErrorCode, PathErrorMessage);
	}

	// Enumerate through the Asset Registry; nothing is loaded here.
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FTopLevelAssetPath BlueprintClassPath = UBlueprint::StaticClass()->GetClassPathName();
	const FTopLevelAssetPath LevelScriptClassPath = ULevelScriptBlueprint::StaticClass()->GetClassPathName();

	TMap<FString, FAssetData> Found;
	TArray<FEntry> Entries;
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

	if (bHasPrefix)
	{
		Registry.ScanPathsSynchronous({Prefix}, true);
		FARFilter Filter;
		Filter.ClassPaths.Add(BlueprintClassPath);
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(FName(*Prefix));
		Filter.bRecursivePaths = true;
		Collect(Filter);
	}
	else
	{
		for (const FString& Requested : RequestedPaths)
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
					FEntry Missing;
					Missing.AssetPath = Requested;
					Missing.Type = TEXT("Unknown");
					Missing.Status = EStatus::NotFound;
					Missing.Reason = TEXT("No Blueprint asset is registered at this path");
					Entries.Add(MoveTemp(Missing));
				}
			}
		}
	}

	int32 ExcludedByType = 0;
	for (const TPair<FString, FAssetData>& Pair : Found)
	{
		const FString Type = FCortexBPAssetOps::DetermineBlueprintTypeFromAssetData(Pair.Value);
		if (bTypeFilter && !Types.Contains(Type))
		{
			++ExcludedByType;
			continue;
		}
		Entries.Add(CompileOne(Pair.Value, Type));
	}
	Algo::SortBy(Entries, &FEntry::AssetPath, [](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });

	int32 Counts[5] = {0, 0, 0, 0, 0}; // success, warning, error, skipped, not_found
	TArray<TSharedPtr<FJsonValue>> AssetValues;
	TArray<TSharedPtr<FJsonValue>> FirstFailures;
	for (const FEntry& Entry : Entries)
	{
		++Counts[static_cast<int32>(Entry.Status)];
		AssetValues.Add(MakeShared<FJsonValueObject>(ToJson(Entry, /*bWithDiagnostics=*/true)));
		if (Entry.Status == EStatus::Error && FirstFailures.Num() < MaxFirstFailures)
		{
			FirstFailures.Add(MakeShared<FJsonValueObject>(ToJson(Entry, /*bWithDiagnostics=*/false)));
		}
	}

	TSharedPtr<FJsonObject> Totals = MakeShared<FJsonObject>();
	Totals->SetNumberField(TEXT("total"), Entries.Num());
	Totals->SetNumberField(TEXT("success"), Counts[0]);
	Totals->SetNumberField(TEXT("warning"), Counts[1]);
	Totals->SetNumberField(TEXT("error"), Counts[2]);
	Totals->SetNumberField(TEXT("skipped"), Counts[3]);
	Totals->SetNumberField(TEXT("not_found"), Counts[4]);
	Totals->SetNumberField(TEXT("excluded_by_type"), ExcludedByType);

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	if (bHasPrefix)
	{
		Report->SetStringField(TEXT("path_prefix"), Prefix);
	}
	else
	{
		TArray<TSharedPtr<FJsonValue>> PathValues;
		for (const FString& Requested : RequestedPaths)
		{
			PathValues.Add(MakeShared<FJsonValueString>(Requested));
		}
		Report->SetArrayField(TEXT("paths"), PathValues);
	}
	if (bTypeFilter)
	{
		TArray<TSharedPtr<FJsonValue>> TypeValues;
		for (const FString& Type : KnownTypes())
		{
			if (Types.Contains(Type))
			{
				TypeValues.Add(MakeShared<FJsonValueString>(Type));
			}
		}
		Report->SetArrayField(TEXT("types"), TypeValues);
	}
	Report->SetObjectField(TEXT("totals"), Totals);
	Report->SetArrayField(TEXT("assets"), AssetValues);

	const FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedOutPath, Report);
	if (!WriteResult.bWritten)
	{
		return FCortexCommandRouter::Error(
			WriteResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : WriteResult.ErrorCode, WriteResult.ErrorMessage);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("out_path"), ResolvedOutPath.AbsolutePath);
	Data->SetObjectField(TEXT("totals"), Totals);
	Data->SetArrayField(TEXT("first_failures"), FirstFailures);
	return FCortexCommandRouter::Success(Data);
}
