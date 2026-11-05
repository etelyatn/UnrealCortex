#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "CortexCommandRouter.h"
#include "CortexSafeFileContract.h"

class FJsonObject;

/**
 * Validated request shared by the bulk Blueprint commands (blueprint.compile_all, blueprint.export_migration_analysis):
 * exactly one of path_prefix / paths, an optional types filter and a resolved out_path.
 */
struct FCortexBPEnumerationRequest
{
	bool bHasPrefix = false;
	/** Normalised mounted folder without a trailing slash (bHasPrefix). */
	FString Prefix;
	/** Asset paths exactly as the caller spelled them (!bHasPrefix). */
	TArray<FString> RequestedPaths;
	bool bTypeFilter = false;
	/** Canonical names from FCortexBPAssetOps::GetBlueprintTypeNames. */
	TSet<FString> Types;
	FCortexResolvedFilePath OutPath;
};

struct FCortexBPEnumeratedAsset
{
	FAssetData Asset;
	/** Blueprint type from the registry tags (no load). */
	FString Type;
};

struct FCortexBPEnumeration
{
	/** Matching Blueprints sorted by object path; level script Blueprints never appear. */
	TArray<FCortexBPEnumeratedAsset> Assets;
	/** Requested paths with no registered Blueprint, in request order. */
	TArray<FString> NotFound;
	/** Found Blueprints dropped by the types filter. */
	int32 ExcludedByType = 0;
};

/** Request validation and Asset Registry enumeration (nothing is loaded) of the bulk Blueprint commands. */
class FCortexBPAssetEnumeration
{
public:
	/**
	 * Validate Params: exactly one of path_prefix (folder) / paths (asset paths), optional types (shared Blueprint type
	 * vocabulary, case-insensitive) and required out_path (FCortexSafeFileContract::ResolveWritePath). On failure OutError
	 * carries the refusal and nothing has been touched.
	 */
	static bool ParseRequest(
		const TSharedPtr<FJsonObject>& Params, FCortexBPEnumerationRequest& OutRequest, FCortexCommandResult& OutError);

	/** Enumerate the request through the Asset Registry, scanning the folder or the missing packages on demand. */
	static FCortexBPEnumeration Enumerate(const FCortexBPEnumerationRequest& Request);

	/** Echo path_prefix / paths / types of the request into a report document. */
	static void WriteRequestFields(const FCortexBPEnumerationRequest& Request, FJsonObject& Report);
};
