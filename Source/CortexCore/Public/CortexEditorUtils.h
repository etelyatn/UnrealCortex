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
};
