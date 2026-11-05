#pragma once

#include "CoreMinimal.h"
#include "Kismet2/KismetEditorUtilities.h"

class FJsonObject;
class UBlueprint;
struct FCortexCommandResult;

/**
 * Shared skeleton for guarded Blueprint migration commands.
 *
 * Sequence: refuse blocked assets, check expected_fingerprint, snapshot, then (unless dry_run) one transaction,
 * journal, Capture, Apply, at most one compile, Verify (readback). Any failure restores the Blueprint: an explicit undo of
 * the guarded transaction, then a reconcile from the generic journal (parent, NewVariables, interfaces, a DuplicateObject
 * copy of every graph and the SCS/ICH template properties), then the caller's Restore for operation-owned state, then
 * exactly one recovery compile (same options as the failed compile when the parent changed in the run).
 * "restored" is reported only when the #124 snapshot, the migration fingerprint (graph_authoring_hash included), the
 * generated-state digest (when compiling) and the caller's DigestExtra all equal the pre-call values; otherwise the asset
 * is blocked through FCortexAssetMutationGuard.
 *
 * FScopedTransaction::Cancel() only discards the undo record; it restores nothing, so it is never the recovery.
 *
 * The outcome object (Data on success, ErrorDetails on failure) carries: dry_run, apply_status
 * (dry_run|applied|failed), changed, compile_status, error_count, warning_count, diagnostics, readback_status
 * (matched|mismatched|skipped), rollback_status (not_needed|restored|unverified), saved, save_status, blocked,
 * dirty_before, dirty_after, fingerprint_before, fingerprint_after.
 */
class FCortexBPGuardedRun
{
public:
	struct FSpec
	{
		FString OpName;
		UBlueprint* Blueprint = nullptr;
		bool bDryRun = false;
		bool bCompile = true;
		bool bSave = false;
		EBlueprintCompileOptions CompileOptions = EBlueprintCompileOptions::None;
		/** Optional expected_fingerprint object, checked through FCortexBPMigrationGuard. */
		TSharedPtr<FJsonObject> Expected;
		/** Snapshot sections compared to prove a restoration; empty selects every section. */
		TSet<FString> SnapshotSections;

		/** Optional: records operation-owned state (binding paths, local variables, bound-event names, instance values) before Apply. */
		TFunction<bool(UBlueprint* Blueprint, FString& OutError)> Capture;

		/** Mutates the Blueprint. Returns false with OutError to fail the run. Not called on dry_run. */
		TFunction<bool(UBlueprint* Blueprint, FString& OutError)> Apply;

		/** Reads the result back; bCompiled tells whether the compiled state may be inspected. Not called on dry_run. */
		TFunction<bool(UBlueprint* Blueprint, bool bCompiled, FString& OutError)> Verify;

		/** Optional: puts the state recorded by Capture back; runs after the generic restore, before the recovery compile. */
		TFunction<bool(UBlueprint* Blueprint, FString& OutError)> Restore;

		/** Optional: digest of operation-owned state, compared before the run and after a restoration. */
		TFunction<FString(UBlueprint* Blueprint)> DigestExtra;
	};

	static FCortexCommandResult Execute(const FSpec& Spec);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook run after the restoration and before it is verified (fault injection). */
	static void SetPostRestoreHookForTest(TFunction<void(UBlueprint*)> Hook);
	static void ClearPostRestoreHookForTest();

	/** Test hook: skip the editor undo so the generic journal alone has to restore. */
	static void SetSkipUndoForTest(bool bSkip);

	/** Test hook: the save step answers SAVE_FAILED without touching the disk (the engine treats a real save failure as fatal). */
	static void SetForceSaveFailureForTest(bool bForce);
#endif
};
