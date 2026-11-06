#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPCleanupOps
{
public:
	/**
	 * Clean up a Blueprint after C++ migration, on the guarded-run skeleton.
	 *
	 * Params: asset_path (required), new_parent_class?, remove_variables?, remove_functions?, migrated_overrides?,
	 *         compile?=true, save?=true, dry_run?, expected_fingerprint?, remove_referencing_nodes?,
	 *         acknowledged_collisions?, acknowledged_losses?, expected_cdo_values?.
	 *
	 * A reparent runs the reparent preflight (`FCortexBPReparentOps::BuildPlan`): blocking collisions refuse
	 * NAME_COLLISION, the non-blocking ones need acknowledged_collisions, and the compile carries the editor's delta
	 * flags so the new native parent's defaults win. Variables a native member replaces are reported as
	 * `adopted_by_engine` (their nodes keep resolving by name); a same-named incompatible variable is renamed by the
	 * engine and removed by its pre-captured GUID. The CDO value of every adopted variable is read back before and
	 * after ({before, after, preserved}) and a difference fails closed with READBACK_MISMATCH. A finalize-only call
	 * (no mutation params) still compiles, verifies `expected_cdo_values` and saves.
	 */
	static FCortexCommandResult CleanupMigration(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Recompile all Blueprints that depend on the given Blueprint.
	 * Params: asset_path (string)
	 */
	static FCortexCommandResult RecompileDependents(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Remove an SCS (Simple Construction Script) component node from a Blueprint.
	 * Used when migrating Blueprint-layer components to C++ UPROPERTY members.
	 * Params: asset_path (string), component_name (string),
	 *         compile (bool, optional, default true)
	 */
	static FCortexCommandResult RemoveSCSComponent(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Rename an SCS (Simple Construction Script) component node on a Blueprint.
	 * Params: asset_path (string), old_name (string), new_name (string),
	 *         compile (bool, optional, default true)
	 */
	static FCortexCommandResult RenameSCSComponent(const TSharedPtr<FJsonObject>& Params);

#if WITH_DEV_AUTOMATION_TESTS
	static void SetRemoveSCSComponentMidflightTestHook(TFunction<void(class USCS_Node*, class UBlueprint*)> InHook);
	static void SetRemoveSCSComponentPostCompileTestHook(TFunction<void(class UBlueprint*)> InHook);
	static void SetRenameSCSComponentPostCompileTestHook(TFunction<void(class UBlueprint*)> InHook);

	/** Test hook: runs in the cleanup readback, after the compile, so a test can force a CDO divergence. */
	static void SetCdoDivergenceTestHook(TFunction<void(class UBlueprint*)> InHook);
#endif
};
