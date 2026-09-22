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

/**
 * Durable identities of everything a prepared patch created or selected.
 *
 * Readback and recovery re-resolve these identities instead of reusing transient UObject, member
 * or pin pointers, because a target compile may reconstruct nodes and pins.
 */
struct FCortexGraphPatchLocators
{
	FGuid GraphGuid;
	FString SubgraphPath;
	TMap<FString, FGuid> NodeGuidByClientId;
	FGuid EntryNodeGuid;
	bool bHasEntryNode = false;
};

/**
 * Honest phase-by-phase outcome of a coordinated patch execution.
 *
 * ApplyStatus:     not_requested | unchanged | applied | failed
 * CompileStatus:   not_requested | compiled | failed
 * ReadbackStatus:  not_requested | matched | mismatched
 * RollbackStatus:  not_requested | restored | unverified
 */
struct FCortexGraphPatchOutcome
{
	FString PatchId;
	bool bChanged = false;
	FString ApplyStatus = TEXT("not_requested");
	FString CompileStatus = TEXT("not_requested");
	FString ReadbackStatus = TEXT("not_requested");
	FString RollbackStatus = TEXT("not_requested");
	int32 TargetCompileCount = 0;
	int32 RecoveryCompileCount = 0;
	bool bSaved = false;
	bool bBlocked = false;
	TArray<FString> Diagnostics;
	FCortexGraphPatchLocators Locators;
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

	/**
	 * Non-mutating apply eligibility predicate: the asset must have a ready class context, must not
	 * already be in a compiler-error state and must not be inside an active play or simulate session.
	 * Pre-existing broken assets are never silently compiled to make them eligible.
	 */
	static bool ValidateEligibility(UBlueprint* Blueprint, FCortexCommandResult& OutError);

	/**
	 * Runs the full apply coordinator for a non-preview request: prepare, eligibility, reversible
	 * apply, optional single target compile, locator re-resolution and authoritative native
	 * readback. Reports honest phase statuses and compile counts. Never saves.
	 */
	static bool Execute(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Params,
		FCortexGraphPatchOutcome& OutOutcome,
		FCortexCommandResult& OutError);
	#if WITH_AUTOMATION_TESTS
	/** Test-only deterministic fault seam; never accepts external command input. */
	static void SetApplyFaultPointForTesting(FName Point);
	static void ClearApplyFaultPointForTesting();
	/** Test-only readback divergence seam (canonical dimension name); never accepts command input. */
	static void SetReadbackFaultForTesting(FName Field);
	/** Test-only observer of real coordinator operations ("target_compile", "recovery_compile"). */
	static void SetOperationObserverForTesting(TFunction<void(FName, UBlueprint*)> Observer);
	static void ClearOperationObserverForTesting();
	#endif
};
