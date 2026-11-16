#pragma once

#include "CoreMinimal.h"

class UObject;

/** Shared editor utility functions for the UnrealCortex plugin.
 *  For PIE-specific utilities (GetPIEWorld, FindActorInPIE, PIENotActiveError),
 *  use FCortexPIEUtils from CortexEditor module instead. */
class CORTEXCORE_API FCortexEditorUtils
{
public:
	/** Notify the editor that an asset was modified via MCP.
	 *  Broadcasts asset update events so the Content Browser and open editors refresh. */
	static void NotifyAssetModified(UObject* Asset);

	/** Normalize a virtual content path, defaulting relative paths to /Game. */
	static FString NormalizeMountedContentPath(const FString& InPath);

	/** Return true if the path targets a writable mounted content root. */
	static bool IsWritableMountedContentPath(const FString& InPath, FString& OutError);

	/** Validate the long package name of a new asset before CreatePackage: a valid long package name as given,
	 *  under a writable mounted root, mapped to a file. On success OutFilename is the .uasset filename to save to. */
	static bool ValidateNewAssetPackageName(const FString& PackageName, FString& OutFilename, FString& OutError);

	/** Return mounted content roots that Cortex write operations may target. */
	static TArray<FString> GetWritableMountedContentRoots();

	/** Delete the given loaded assets in one ObjectTools::ForceDeleteObjects call, then remove the
	 *  package file of every asset the editor actually deleted. Assets the editor refused (for
	 *  example because an editor vetoed the delete) keep their object and their file.
	 *
	 *  The returned array is aligned with Assets: an empty string means that asset's object was
	 *  deleted and its package file is gone, otherwise the string describes why the asset and its
	 *  file were left alone. One failing asset never prevents the other assets from being deleted.
	 *
	 *  Must be called on the game thread with the editor loaded. */
	static TArray<FString> DeleteLoadedAssets(const TArray<UObject*>& Assets);

	/** Return the sorted, unique package names that reference any package in PackageNames from
	 *  outside that set. Only package dependencies count (an asset must really reference the package);
	 *  AssetManager management edges are ignored, so a primary asset rule or label never blocks a
	 *  delete. References between packages inside the set are not reported, so deleting a folder is
	 *  not blocked by its own assets referencing each other. */
	static TArray<FString> GetExternalReferencers(const TArray<FName>& PackageNames);

#if WITH_DEV_AUTOMATION_TESTS
	static void AddTestWritableContentRoot(const FString& InRoot);
	static void RemoveTestWritableContentRoot(const FString& InRoot);
#else
	static void AddTestWritableContentRoot(const FString&) {}
	static void RemoveTestWritableContentRoot(const FString&) {}
#endif
};
