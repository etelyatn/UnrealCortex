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

	using FPIEPoll = TFunction<TOptional<FCortexCommandResult>(UWorld& PIEWorld)>;

	/**
	 * Completes a deferred command by polling on the core ticker every IntervalSeconds (0: every frame) while World remains
	 * the active PIE world. Paused frames are skipped. The first result Poll returns answers the command; if PIE
	 * ends or is replaced first, the command answers PIE_TERMINATED "PIE terminated during {OperationLabel}".
	 * Returns the deferred result whose transport deadline covers OperationSeconds plus a grace period.
	 */
	static FCortexCommandResult StartDeferredPIEPoll(
		UWorld* World,
		float IntervalSeconds,
		double OperationSeconds,
		const FString& OperationLabel,
		FDeferredResponseCallback DeferredCallback,
		FPIEPoll Poll);
};
