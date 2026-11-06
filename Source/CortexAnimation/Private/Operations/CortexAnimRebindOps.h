#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;
class UAnimBlueprint;

/**
 * anim.rebind_function: retargets a Blueprint function to a function the Blueprint or its parent classes declare, in the anim-node places
 * the engine's own rename hook reaches — function-typed property bindings — and in the three public function slots the hook skips
 * (AnimGraphNode_Base.cpp:1097-1103). K2 call nodes are never touched; blueprint.adopt_native_member owns variables.
 *
 * Params: asset_path, from, to, expected_fingerprint (required), dry_run? (default true), compile? (default true), save? (default false).
 *
 * The plan is made of exact occurrences: each binding-map entry and each function slot that holds the source function owned by this
 * Blueprint. A same-named function on an unrelated class and any pre-existing use of the destination are preserved and reported.
 *
 * Every failure after the first mutation (compile, readback, save) takes one route: restore the captured journal (the whole binding map
 * of every anim node, the three slot FMemberReferences, the K2 call references), cancel the edit's undo record, run exactly one recovery
 * compile, verify the journal's content, restore the dirty flag only once verified, and block the asset path otherwise.
 */
class FCortexAnimRebindOps
{
public:
	static FCortexCommandResult RebindFunction(const TSharedPtr<FJsonObject>& Params);

	/** Where fault injection can intervene. */
	enum class EPhase : uint8
	{
		/** After the edits, before the exact edit verification and the compile: true fails the operation here; the hook may also corrupt state to be caught by the verification. */
		AfterMutation,
		/** In place of the compile: true simulates a compile failure. */
		Compile,
		/** Before the readback verdict: true simulates a readback mismatch. */
		Readback,
		/** In place of the save: true simulates a failed save. */
		Save,
		/** After the journal restore, before its verification: the hook may corrupt state; true marks the restore step as failed. */
		AfterRestore,
	};

#if WITH_AUTOMATION_TESTS
	/** Test hook: called at each EPhase; its result injects a failure of that phase. */
	static void SetFaultHookForTest(TFunction<bool(EPhase, UAnimBlueprint*)> Hook);
	static void ClearFaultHookForTest();
#endif
};
