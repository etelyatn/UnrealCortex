#pragma once

#include "CoreMinimal.h"

class UObject;

/**
 * Process-lifetime fail-closed guard for assets whose recovery could not be verified.
 * A block lasts until the package is reloaded from disk (core.reload_asset) or the Editor exits.
 */
class CORTEXCORE_API FCortexAssetMutationGuard
{
public:
	static void Block(const UObject* Asset, const FString& Reason);
	static bool IsBlocked(const UObject* Asset, FString& OutReason);
	static bool IsPathBlocked(const FString& AssetPath, FString& OutReason);
	/** Clears every block inside the package; call only once its in-memory state was replaced from disk. Returns true if a block was cleared. */
	static bool ClearPackage(const FString& PackageName);
};
