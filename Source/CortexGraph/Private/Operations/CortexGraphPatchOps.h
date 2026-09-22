#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UBlueprint;
class FJsonObject;

/**
 * Durable result of a complete, non-mutating graph patch preflight.
 *
 * The prepared patch deliberately contains only JSON descriptors, paths, GUIDs and names. It
 * does not retain UObjects, reflected members or pins because all of those can be reconstructed
 * by the apply phase after the final guard.
 */
struct FCortexGraphPreparedPatch
{
	TSharedPtr<FJsonObject> NormalizedRequest;
	TSharedPtr<FJsonObject> FingerprintBefore;
	FString ValidationHash;
	FString PatchId;
	FString GraphGuid;
	FString SubgraphPath;
	bool bChanged = false;
	bool bDryRun = true;
	bool bCompile = true;
	bool bSave = false;
	TArray<FString> PlannedNodeIds;
	TArray<FString> PlannedConnectionKeys;

	/** Prepared state never owns transient UObject pointers. */
	bool HasTransientObjects() const { return false; }
};

class FCortexGraphPatchOps
{
public:
	/** Performs strict, game-thread-only validation without changing the target Blueprint. */
	static bool Preflight(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Params,
		FCortexGraphPreparedPatch& OutPrepared,
		FCortexCommandResult& OutError);

	/**
	 * Applies a prepared patch as one editor transaction. The prepared plan is revalidated
	 * against the live asset immediately before the first mutation.
	 */
	static bool Apply(
		UBlueprint* Blueprint,
		const FCortexGraphPreparedPatch& Prepared,
		FCortexCommandResult& OutError);
	#if WITH_AUTOMATION_TESTS
	/** Test-only deterministic fault seam; never accepts external command input. */
	static void SetApplyFaultPointForTesting(FName Point);
	static void ClearApplyFaultPointForTesting();
	#endif
};
