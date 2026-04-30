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

	/** Normalize a content path for asset operations.
	 *  Relative paths (no leading /) get /Game/ prepended for backwards compatibility.
	 *  Absolute paths are kept as-is, trusting UE's mount point system to resolve
	 *  plugin content roots (e.g. /PluginName/) correctly. */
	static FString NormalizeMountedPath(const FString& InPath);
};
