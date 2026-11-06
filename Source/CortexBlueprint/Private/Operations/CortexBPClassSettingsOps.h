#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "CortexCommandRouter.h"

class UBlueprint;

/**
 * Blueprint Class Settings operations.
 * Manages structural settings: interfaces, tick configuration, replication.
 */
class FCortexBPClassSettingsOps
{
public:
	/**
	 * Add an interface implementation to a Blueprint.
	 * Params: asset_path (string), interface_path (string), compile (bool, default true)
	 */
	static FCortexCommandResult AddInterface(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Preview or remove an interface implementation from a Blueprint.
	 * Params: asset_path (string), interface_path (string), preserve_implementations (bool, required: true keeps function
	 *         graphs as ordinary functions and events as custom events, false accepts the loss), dry_run (bool, default false),
	 *         compile (bool, default true), expected_fingerprint + expected_validation_hash (required to apply; read from a dry run).
	 * Apply verifies the predicted disposition on the live graphs before compiling and restores explicitly on a mismatch.
	 */
	static FCortexCommandResult RemoveInterface(const TSharedPtr<FJsonObject>& Params);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test seam: the pre-compile disposition verifier reports a mismatch (exercises the explicit restore). */
	static void SetDispositionMismatchForTest(bool bForce);
#endif

	/**
	 * Set Actor tick settings on a Blueprint CDO.
	 * Params: asset_path (string), start_with_tick_enabled (bool), can_ever_tick (bool),
	 *         tick_interval (number), compile (bool), save (bool)
	 */
	static FCortexCommandResult SetTickSettings(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Set replication settings on a Blueprint CDO.
	 * Params: asset_path (string), replicates (bool), replicate_movement (bool),
	 *         net_dormancy (string), net_use_owner_relevancy (bool), compile (bool), save (bool)
	 */
	static FCortexCommandResult SetReplicationSettings(const TSharedPtr<FJsonObject>& Params);

private:
	/** Resolve an interface class from a path string (short name, full path, or Blueprint path). */
	static UClass* ResolveInterfaceClass(const FString& InterfacePath, FString& OutError);
};
