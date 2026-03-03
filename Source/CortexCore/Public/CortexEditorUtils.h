#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class AActor;
class UObject;
class UWorld;

/** Shared editor utility functions for the UnrealCortex plugin */
class CORTEXCORE_API FCortexEditorUtils
{
public:
	/** Notify the editor that an asset was modified via MCP.
	 *  Broadcasts asset update events so the Content Browser and open editors refresh. */
	static void NotifyAssetModified(UObject* Asset);

	/** Returns the active PIE world, or nullptr if PIE is not running. */
	static UWorld* GetPIEWorld();

	/**
	 * Find an actor in the PIE world by label, name, or full path.
	 * Matches in order: ActorLabel, GetName(), GetPathName().
	 * Returns nullptr if not found or world is null.
	 */
	static AActor* FindActorInPIE(UWorld* PIEWorld, const FString& ActorIdentifier);

	/** Returns a standard PIENotActive error result with recovery hint. */
	static FCortexCommandResult PIENotActiveError();
};
