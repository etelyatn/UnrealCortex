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
	/**
	 * Durable `replace_entry` plan when the request selected the migration shell. It carries only
	 * names, GUIDs and canonical descriptors, so apply, readback and recovery rebuild every pointer.
	 */
	TSharedPtr<FJsonObject> MigrationPlan;
	/**
	 * Durable `copy_subgraph` / `move_subgraph` plan when the request selected the transfer shell.
	 * Like the replacement plan it carries only names, GUIDs and canonical captures.
	 */
	TSharedPtr<FJsonObject> TransferPlan;
	/**
	 * Durable `prune_island` plan when the request selected the prune shell: the approved removal set,
	 * the published partition and the preservation contract of the retained body.
	 */
	TSharedPtr<FJsonObject> PrunePlan;
	/** Durable `retire_entries` plan when the request selected the compile-invalid retirement shell. */
	TSharedPtr<FJsonObject> RetirementPlan;
	/**
	 * True when an accepted replay named a source locator that no longer exists. The apply consumed
	 * the stale entry and that provenance is not inventoried, so it is reported instead of inferred.
	 */
	bool bReplayedWithAbsentSource = false;
	bool bIsMigration() const { return MigrationPlan.IsValid(); }
	bool bIsTransfer() const { return TransferPlan.IsValid(); }
	bool bIsPrune() const { return PrunePlan.IsValid(); }
	bool bIsRetirement() const { return RetirementPlan.IsValid(); }

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
	bool bReplayedWithAbsentSource = false;
	/** Live fingerprint of the target asset before the coordinated patch and after it terminates. */
	TSharedPtr<FJsonObject> FingerprintBefore;
	TSharedPtr<FJsonObject> FingerprintAfter;
	bool bDirtyBefore = false;
	bool bDirtyAfter = false;
	/** Client ids whose deterministic nodes were reused instead of created. */
	TArray<FString> ReusedClientIds;
	/**
	 * Bounded inventory of a transfer request (crossing edges, boundary mappings, dependencies and
	 * the removal set), published with the preview and the apply result so a caller never has to read
	 * the durable plan to learn which edges need explicit boundary mappings.
	 */
	TSharedPtr<FJsonObject> TransferInventory;
	/**
	 * Bounded inventory of a prune request (removable, shared, blocked and external-edge partitions
	 * plus the scan counts and completeness), published with the preview and the apply result so a
	 * caller approves the removable set from the response instead of reading the durable plan.
	 */
	TSharedPtr<FJsonObject> PruneInventory;
	/** Bounded inventory of a retirement request, published with preview and apply results. */
	TSharedPtr<FJsonObject> RetirementInventory;
	TArray<FString> Diagnostics;
	FCortexGraphPatchLocators Locators;
};

class FCortexGraphPatchOps
{
public:
	/**
	 * The published `max_scanned_nodes` bound of one graph-wide scan: the whole-asset node count a
	 * patch request may address and the scan budget a bounded migration traversal may spend. It is
	 * published by `graph.get_authoring_context` and is the one source of truth for both.
	 */
	static constexpr int32 MaxScannedNodes = 2048;

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
	static bool ValidateEligibility(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Params,
		FCortexCommandResult& OutError);

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

	/**
	 * Shared request-validation primitives. The migration shell validates its own object against the
	 * same strict rules the authoring envelope uses, so both shells refuse identical malformed input
	 * with identical errors instead of growing a second validation scheme.
	 */
	static bool HasOnlyFields(
		const TSharedPtr<FJsonObject>& Object,
		const TSet<FString>& Allowed,
		FCortexCommandResult& OutError,
		const FString& Context);
	static bool ReadStrictBool(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Field,
		bool DefaultValue,
		bool& OutValue,
		FCortexCommandResult& OutError);
	static bool ReadRequiredString(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Field,
		FString& OutValue,
		FCortexCommandResult& OutError);
	static bool ParseGuidField(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Field,
		FGuid& OutGuid,
		FCortexCommandResult& OutError);
	/** Resolves one canonical graph reference to a mutable user graph, subgraph included. */
	static bool ResolveGraphByGuid(
		UBlueprint* Blueprint,
		const FGuid& GraphGuid,
		const FString& SubgraphPath,
		UEdGraph*& OutGraph,
		FCortexCommandResult& OutError);
	/**
	 * Published kind string of the one graph a `graph_guid` names, root or nested composite child.
	 * This is the single identity resolution behind every optional `graph_kind` consistency check, so
	 * a nested target is compared against its own owning graph's kind instead of being skipped. It
	 * fails with the same identity errors ResolveGraphByGuid raises: GRAPH_NOT_FOUND when no graph
	 * matches and INVALID_FIELD when the GUID identifies more than one.
	 */
	static bool ResolveGraphKindByGuid(
		UBlueprint* Blueprint,
		const FGuid& GraphGuid,
		FString& OutKind,
		FCortexCommandResult& OutError);
	/**
	 * Deterministic node identity of one planned identity seed: a stable hash of the canonical patch
	 * GUID and the seed. Authoring client ids, replacement terminators and transfer destinations all
	 * derive through this one function, so no second identity scheme can appear.
	 */
	static FGuid DeriveNodeGuid(const FString& PatchId, const FString& ClientId);
	/** Canonical planned signature descriptor of one native pin. */
	static TSharedPtr<FJsonObject> MakePinSignatureDescriptor(const UEdGraphPin& Pin);
	/**
	 * Canonical signature descriptor of one bare pin type (a declared variable's type), so authored
	 * variable types compare through the same canonical scheme as native pins.
	 */
	static TSharedPtr<FJsonObject> MakePinSignatureDescriptorForType(const FEdGraphPinType& PinType);
	/** Canonical string of one planned pin signature descriptor. */
	static FString CanonicalPinSignature(const TSharedPtr<FJsonObject>& Descriptor);
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
