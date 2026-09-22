#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UBlueprint;
class FJsonObject;
class FCompilerResultsLog;

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
	/**
	 * Deterministic identity of every planned client id: f(patch_id, client_id). Known before the
	 * first mutation so preview, apply, readback and caller inspection agree without any receipt.
	 */
	TMap<FString, FGuid> NodeGuidByClientId;
	/** Durable locator of an implementation entry node that already existed before this patch. */
	FGuid EntryNodeGuid;
	bool bHasEntryNode = false;
	/** Client ids whose deterministic nodes already exist and match the canonical planned intent. */
	TArray<FString> ReusedClientIds;
	/** True when the whole planned intent is already present exactly: an idempotent replay. */
	bool bFullyReused = false;
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
 * SaveStatus:      not_requested | saved | failed
 * PostSaveStatus:  not_requested | verified | failed
 *
 * Persistence is reported independently of the in-memory result: a save failure keeps
 * ApplyStatus="applied" and never claims a rollback, and a post-save verification failure keeps
 * SaveStatus="saved" because the disk commit really happened.
 */
struct FCortexGraphPatchOutcome
{
	FString PatchId;
	bool bChanged = false;
	FString ApplyStatus = TEXT("not_requested");
	FString CompileStatus = TEXT("not_requested");
	FString ReadbackStatus = TEXT("not_requested");
	FString RollbackStatus = TEXT("not_requested");
	FString SaveStatus = TEXT("not_requested");
	FString PostSaveStatus = TEXT("not_requested");
	int32 TargetCompileCount = 0;
	int32 RecoveryCompileCount = 0;
	bool bSaved = false;
	bool bBlocked = false;
	/** Live fingerprint of the target asset before the coordinated patch and after it terminates. */
	TSharedPtr<FJsonObject> FingerprintBefore;
	TSharedPtr<FJsonObject> FingerprintAfter;
	bool bDirtyBefore = false;
	bool bDirtyAfter = false;
	/** Client ids whose deterministic nodes were reused instead of created. */
	TArray<FString> ReusedClientIds;
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
	 * apply, optional single target compile, locator re-resolution, authoritative native readback
	 * and, only when the prepared request asked for it, exactly one save of the target package
	 * followed by post-save persistence verification. Reports honest apply, compile, readback,
	 * rollback and persistence statuses. Never reloads the asset and never saves a no-op.
	 */
	static bool Execute(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Params,
		FCortexGraphPatchOutcome& OutOutcome,
		FCortexCommandResult& OutError);

	/**
	 * Appends bounded compiler diagnostics: at most 16 entries, reserving one slot for the
	 * omission marker, and every entry at most 512 characters including any elision suffix.
	 */
	static void CollectCompilerDiagnostics(const FCompilerResultsLog& Log, TArray<FString>& OutDiagnostics);

	/**
	 * Enforces one shared final diagnostics bound over a whole outcome: at most 16 entries with a
	 * single omission marker, and at most 512 characters per entry including any suffix.
	 */
	static void TrimDiagnostics(TArray<FString>& InOutDiagnostics);
	#if WITH_AUTOMATION_TESTS
	/** Test-only deterministic fault seam; never accepts external command input. */
	static void SetApplyFaultPointForTesting(FName Point);
	static void ClearApplyFaultPointForTesting();
	/** Test-only readback divergence seam (canonical dimension name); never accepts command input. */
	static void SetReadbackFaultForTesting(FName Field);
	/**
	 * Test-only native-state mutation between apply and readback, used to prove readback compares
	 * real native state instead of the planned request. Never accepts external command input.
	 */
	static void SetPreReadbackMutatorForTesting(TFunction<void(UBlueprint*)> Mutator);
	static void ClearPreReadbackMutatorForTesting();
	/** Test-only observer of real coordinator operations ("target_compile", "recovery_compile"). */
	static void SetOperationObserverForTesting(TFunction<void(FName, UBlueprint*)> Observer);
	static void ClearOperationObserverForTesting();
	/**
	 * Test-only persistence fault seams; never accepts external command input and never reachable
	 * from patch JSON. The save seam makes the single target save report failure, the post-save seam
	 * fails exactly one named post-save persistence check.
	 */
	static void SetSaveFaultForTesting(bool bFail);
	static void SetPostSaveVerificationFaultForTesting(FName Check);
	#endif
};
