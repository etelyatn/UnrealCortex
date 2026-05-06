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

	/** Return mounted content roots that Cortex write operations may target. */
	static TArray<FString> GetWritableMountedContentRoots();

#if WITH_DEV_AUTOMATION_TESTS
	static void AddTestWritableContentRoot(const FString& InRoot);
	static void RemoveTestWritableContentRoot(const FString& InRoot);
#else
	static void AddTestWritableContentRoot(const FString&) {}
	static void RemoveTestWritableContentRoot(const FString&) {}
#endif
};
