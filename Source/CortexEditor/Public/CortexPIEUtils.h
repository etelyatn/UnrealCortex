#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class AActor;
class UWorld;

/**
 * Shared PIE utility functions for CortexMaterial, CortexQA, and other modules
 * that need runtime access to the Play-In-Editor world.
 */
class CORTEXEDITOR_API FCortexPIEUtils
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
