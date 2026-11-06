#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UBlueprint;
struct FCortexCommandResult;

/**
 * Stale-write guard for migration commands.
 *
 * The fingerprint covers what migrations change and the graph authoring fingerprint does not: parent class,
 * member variables (including uncompiled default text), the generated class CDO values, SCS/inherited component
 * template properties and implemented interfaces. It deliberately excludes the dirty flag so a dry run and the apply
 * that follows it compare equal.
 *
 * Shape: {graph_authoring_version, graph_authoring_hash, migration_state_hash}.
 */
class FCortexBPMigrationGuard
{
public:
	static TSharedPtr<FJsonObject> ComputeFingerprint(UBlueprint* Blueprint);

	/**
	 * Accepts the current fingerprint. Refuses an unknown field (INVALID_FIELD), a missing or empty
	 * migration_state_hash or any mismatching field (STALE_PRECONDITION with details.current_fingerprint).
	 */
	static bool CheckExpected(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Expected, FCortexCommandResult& OutError);
};
