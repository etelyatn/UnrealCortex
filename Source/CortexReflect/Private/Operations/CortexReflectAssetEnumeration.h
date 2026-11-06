#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "CortexCommandRouter.h"
#include "CortexSafeFileContract.h"

class FJsonObject;
class FJsonValue;

/**
 * Request parsing and Asset Registry enumeration shared by the commands that work over `path_prefixes`
 * (export_dependency_graph, find_name_lookups, find_duplicate_graphs). Nothing is loaded here.
 */
namespace CortexReflectAssetEnumeration
{
	/** Assets found under a set of prefixes. */
	struct FEnumeration
	{
		/** Normalised mounted folders without trailing slash, in request order. */
		TArray<FString> Prefixes;
		/** Matching assets sorted by object path (case-sensitive); level script Blueprints never appear. */
		TArray<FAssetData> Assets;
		/** Number of Assets under each prefix, parallel to Prefixes (an asset under overlapping prefixes counts for each). */
		TArray<int32> MatchedPerPrefix;
	};

	/** True when PackageName is the folder Prefix itself or lies below it. */
	bool IsInPrefix(const FString& PackageName, const FString& Prefix);

	/** A mounted folder path: leading slash, no "..", no "//", no '.'. */
	bool IsPlainFolderPath(const FString& Path);

	/**
	 * Reads the required `path_prefixes` array: every entry a non-empty string, normalised as a mounted content path
	 * with trailing slashes dropped and validated as a plain folder path; duplicates are removed.
	 * On failure OutError carries the refusal.
	 */
	bool ParsePathPrefixes(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutPrefixes, FCortexCommandResult& OutError);

	/** Scans the prefixes and returns the assets of ClassPaths (recursive classes) below them, sorted deterministically. */
	FEnumeration Enumerate(const TArray<FString>& Prefixes, const TArray<FTopLevelAssetPath>& ClassPaths);

	/** Writes `matched_assets` ({prefix: count}) and, for every prefix that matched nothing, an entry of `warnings`. */
	void WriteMatchedFields(const FEnumeration& Enumeration, FJsonObject& Json);

	TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Strings);

	/** Writes `load_failures` when there is at least one failure; an empty list is not emitted. */
	void WriteLoadFailures(const TArray<FString>& LoadFailures, FJsonObject& Json);

	/** First entries of an over-long result array that the compact out_path summary keeps. */
	constexpr int32 SummaryEntryLimit = 10;

	/**
	 * Optional `out_path`: absent leaves bOutHasPath false; present it must be a non-empty string that
	 * FCortexSafeFileContract::ResolveWritePath accepts. A refusal is returned before any work is done.
	 */
	bool ParseOptionalOutPath(
		const TSharedPtr<FJsonObject>& Params, bool& bOutHasPath, FCortexResolvedFilePath& OutPath, FCortexCommandResult& OutError);

	/**
	 * Writes Full to OutPath and returns the compact response: every field of Full except the arrays named in
	 * TruncatedArrays is copied as is; each named array keeps its first SummaryEntryLimit entries and gains a
	 * `<name>_total` count; `out_path` is added.
	 */
	FCortexCommandResult WriteReportWithSummary(
		const FCortexResolvedFilePath& OutPath, const TSharedRef<FJsonObject>& Full, const TArray<FString>& TruncatedArrays);
}
