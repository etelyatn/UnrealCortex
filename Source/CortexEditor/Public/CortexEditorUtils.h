#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class AActor;
class UWorld;

/**
 * Shared PIE world access utilities.
 * Used by CortexMaterial (dynamic instances) and CortexQA (game testing).
 */
class CORTEXEDITOR_API FCortexEditorUtils
{
public:
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
