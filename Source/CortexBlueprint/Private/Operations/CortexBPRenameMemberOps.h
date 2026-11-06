#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UBlueprint;
struct FCortexCommandResult;

/**
 * blueprint.rename_member: guarded rename of a Blueprint variable, dispatcher, function or local variable.
 *
 * The engine renames the references it can reach (this Blueprint and loaded dependents) and leaves the rest stale, so the
 * command refuses before any mutation when a dependent Blueprint (loaded, unsaved, or found through the Asset Registry
 * and its derived classes) references the member, when the target name is taken, or when the member is bound to engine
 * state it would silently drop (RepNotify, FieldNotify, override and interface functions, SCS components).
 *
 * Params: asset_path, kind (variable|function|dispatcher|local), old, new, function? (required for local),
 * dry_run?, expected_fingerprint?, compile?=true, save?=false, acknowledged_losses? (`save_game:<variable>` keys).
 */
class FCortexBPRenameMemberOps
{
public:
	static FCortexCommandResult Execute(const TSharedPtr<FJsonObject>& Params);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook: runs inside the apply step after the rename (fault injection). */
	static void SetPostApplyHookForTest(TFunction<void(UBlueprint*)> Hook);
	static void ClearPostApplyHookForTest();
#endif
};
