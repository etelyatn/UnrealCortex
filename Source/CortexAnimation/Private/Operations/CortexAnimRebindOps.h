#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;
class UAnimBlueprint;

/**
 * anim.rebind_function: retargets a Blueprint function to a function the parent class (or the Blueprint) declares, in the anim-node
 * places the engine's own rename hooks reach — function-typed property bindings — and in the three public function slots the engine
 * hooks skip (AnimGraphNode_Base.cpp:1097-1103). K2 call nodes are never touched; core.blueprint.adopt_native_member owns variables.
 *
 * Params: asset_path, from, to, expected_fingerprint (required), dry_run? (default true), compile? (default true), save? (default false).
 * Every failure after the first mutation restores a captured journal, cancels the transaction, runs exactly one recovery compile,
 * restores the dirty flag and verifies by content readback; a failed verification blocks the asset path.
 */
class FCortexAnimRebindOps
{
public:
	static FCortexCommandResult RebindFunction(const TSharedPtr<FJsonObject>& Params);

#if WITH_AUTOMATION_TESTS
	/** Test hook: runs right after the mutation and before the compile; true simulates a compile failure (fault injection). */
	static void SetCompileFailureHookForTest(TFunction<bool(UAnimBlueprint*)> Hook);
	static void ClearCompileFailureHookForTest();
#endif
};
