#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPStructureOps
{
public:
	static FCortexCommandResult AddVariable(const TSharedPtr<FJsonObject>& Params);

	/**
	 * blueprint.remove_variable: guarded removal with a dry_run, a stale-write fingerprint, the #125 reference list and
	 * the SaveGame guard. Refuses MEMBER_REFERENCED while a get/set node names the variable unless
	 * remove_referencing_nodes removes them; refuses POTENTIAL_DATA_LOSS for a SaveGame variable until
	 * acknowledged_losses carries exactly its save_game:<variable> keys.
	 * Params: asset_path, name, dry_run?, expected_fingerprint?, compile?=false, save?=false,
	 *         remove_referencing_nodes?, acknowledged_losses?.
	 */
	static FCortexCommandResult RemoveVariable(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AddFunction(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveGraph(const TSharedPtr<FJsonObject>& Params);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook: runs inside the apply step after the variable was removed (fault injection). */
	static void SetRemoveVariablePostApplyHookForTest(TFunction<void(class UBlueprint*)> Hook);
	static void ClearRemoveVariablePostApplyHookForTest();
#endif
};

