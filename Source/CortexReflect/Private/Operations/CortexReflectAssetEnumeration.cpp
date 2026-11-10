#include "Operations/CortexReflectAssetEnumeration.h"
#include "CortexEngineCompat.h"

#include "Algo/Sort.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CortexEditorUtils.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Modules/ModuleManager.h"

namespace CortexReflectAssetEnumeration
{
bool IsInPrefix(const FString& PackageName, const FString& Prefix)
{
	return PackageName == Prefix || (PackageName.StartsWith(Prefix) && PackageName.Len() > Prefix.Len() && PackageName[Prefix.Len()] == TEXT('/'));
}

bool IsPlainFolderPath(const FString& Path)
{
	return Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT("..")) && !Path.Contains(TEXT("//")) && !Path.Contains(TEXT("."));
}

bool ParsePathPrefixes(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutPrefixes, FCortexCommandResult& OutError)
{
	if (!Params.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PrefixValues = nullptr;
	if (!Params->TryGetArrayField(TEXT("path_prefixes"), PrefixValues) || PrefixValues->Num() == 0)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("path_prefixes must be a non-empty array of mounted folder paths"));
		return false;
	}

	OutPrefixes.Reset();
	for (const TSharedPtr<FJsonValue>& Value : *PrefixValues)
	{
		FString Prefix;
		if (!Value.IsValid() || !Value->TryGetString(Prefix) || Prefix.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				TEXT("path_prefixes must contain only non-empty folder path strings"));
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
				FString::Printf(TEXT("path_prefixes entry is not a valid mounted folder path: %s"), *Prefix));
			return false;
		}
		OutPrefixes.AddUnique(MoveTemp(Prefix));
	}
	return true;
}

FEnumeration Enumerate(const TArray<FString>& Prefixes, const TArray<FTopLevelAssetPath>& ClassPaths)
{
	FEnumeration Result;
	Result.Prefixes = Prefixes;

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	for (const FString& Prefix : Prefixes)
	{
		Registry.ScanPathsSynchronous({Prefix}, true);
	}

	FARFilter Filter;
	Filter.ClassPaths = ClassPaths;
	Filter.bRecursiveClasses = true;
	for (const FString& Prefix : Prefixes)
	{
		Filter.PackagePaths.Add(FName(*Prefix));
	}
	Filter.bRecursivePaths = true;
	Registry.GetAssets(Filter, Result.Assets);

	const FTopLevelAssetPath LevelScriptClassPath = ULevelScriptBlueprint::StaticClass()->GetClassPathName();
	Result.Assets.RemoveAll([&LevelScriptClassPath](const FAssetData& Asset) { return Asset.AssetClassPath == LevelScriptClassPath; });
	Algo::SortBy(Result.Assets, [](const FAssetData& Asset) { return Asset.GetObjectPathString(); },
		[](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });

	Result.MatchedPerPrefix.Init(0, Prefixes.Num());
	for (const FAssetData& Asset : Result.Assets)
	{
		const FString PackageName = Asset.PackageName.ToString();
		for (int32 Index = 0; Index < Prefixes.Num(); ++Index)
		{
			if (IsInPrefix(PackageName, Prefixes[Index]))
			{
				++Result.MatchedPerPrefix[Index];
			}
		}
	}
	return Result;
}

void WriteMatchedFields(const FEnumeration& Enumeration, FJsonObject& Json)
{
	TSharedRef<FJsonObject> Matched = MakeShared<FJsonObject>();
	TArray<FString> Warnings;
	for (int32 Index = 0; Index < Enumeration.Prefixes.Num(); ++Index)
	{
		Matched->SetNumberField(Enumeration.Prefixes[Index], Enumeration.MatchedPerPrefix[Index]);
		if (Enumeration.MatchedPerPrefix[Index] == 0)
		{
			Warnings.Add(FString::Printf(TEXT("path_prefixes entry '%s' matched no assets"), *Enumeration.Prefixes[Index]));
		}
	}
	Json.SetObjectField(TEXT("matched_assets"), Matched);
	if (Warnings.Num() > 0)
	{
		Json.SetArrayField(TEXT("warnings"), StringValues(Warnings));
	}
}

TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Strings.Num());
	for (const FString& String : Strings)
	{
		Values.Add(MakeShared<FJsonValueString>(String));
	}
	return Values;
}

void WriteLoadFailures(const TArray<FString>& LoadFailures, FJsonObject& Json)
{
	if (LoadFailures.Num() > 0)
	{
		Json.SetArrayField(TEXT("load_failures"), StringValues(LoadFailures));
	}
}

bool ParseOptionalOutPath(
	const TSharedPtr<FJsonObject>& Params, bool& bOutHasPath, FCortexResolvedFilePath& OutPath, FCortexCommandResult& OutError)
{
	bOutHasPath = false;
	if (!Params.IsValid() || !Params->HasField(TEXT("out_path")))
	{
		return true;
	}
	FString Requested;
	if (!Params->TryGetStringField(TEXT("out_path"), Requested) || Requested.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("out_path must be a non-empty file path string"));
		return false;
	}
	FString ErrorCode;
	FString ErrorMessage;
	if (!FCortexSafeFileContract::ResolveWritePath(Requested, OutPath, ErrorCode, ErrorMessage))
	{
		OutError = FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
		return false;
	}
	bOutHasPath = true;
	return true;
}

FCortexCommandResult WriteReportWithSummary(
	const FCortexResolvedFilePath& OutPath, const TSharedRef<FJsonObject>& Full, const TArray<FString>& TruncatedArrays)
{
	const FCortexJsonFileWriteResult Written = FCortexSafeFileContract::WriteJsonReportAtomic(OutPath, Full);
	if (!Written.bWritten)
	{
		return FCortexCommandRouter::Error(Written.ErrorCode, Written.ErrorMessage);
	}

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	for (const auto& Field : Full->Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (TruncatedArrays.Contains(CortexEngineCompat::JsonKeyToString(Field.Key)) && Field.Value.IsValid() && Field.Value->TryGetArray(Array))
		{
			TArray<TSharedPtr<FJsonValue>> First;
			for (int32 Index = 0; Index < Array->Num() && Index < SummaryEntryLimit; ++Index)
			{
				First.Add((*Array)[Index]);
			}
			Summary->SetArrayField(CortexEngineCompat::JsonKeyToString(Field.Key), First);
			Summary->SetNumberField(CortexEngineCompat::JsonKeyToString(Field.Key) + TEXT("_total"), Array->Num());
		}
		else
		{
			Summary->SetField(CortexEngineCompat::JsonKeyToString(Field.Key), Field.Value);
		}
	}
	Summary->SetStringField(TEXT("out_path"), OutPath.AbsolutePath);
	return FCortexCommandRouter::Success(Summary);
}
}
