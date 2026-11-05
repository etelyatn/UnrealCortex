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
 * Apply, at most one compile, Verify (readback). Any failure restores the Blueprint from the recorded state
 * (Restore, or by default an explicit undo of the guarded transaction), runs exactly one recovery compile when a compile
 * was requested, and verifies the restoration (snapshot equal to the pre-call snapshot and equal migration
 * fingerprint). An unverifiable recovery blocks the asset through FCortexAssetMutationGuard.
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

		/** Mutates the Blueprint. Returns false with OutError to fail the run. Not called on dry_run. */
		TFunction<bool(UBlueprint* Blueprint, FString& OutError)> Apply;

		/** Reads the result back; bCompiled tells whether the compiled state may be inspected. Not called on dry_run. */
		TFunction<bool(UBlueprint* Blueprint, bool bCompiled, FString& OutError)> Verify;

		/** Optional recovery that puts the Blueprint back; default is an explicit undo of the guarded transaction. */
		TFunction<bool(UBlueprint* Blueprint, FString& OutError)> Restore;
	};

	static FCortexCommandResult Execute(const FSpec& Spec);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook run after the restoration and before it is verified (fault injection). */
	static void SetPostRestoreHookForTest(TFunction<void(UBlueprint*)> Hook);
	static void ClearPostRestoreHookForTest();
#endif
};
