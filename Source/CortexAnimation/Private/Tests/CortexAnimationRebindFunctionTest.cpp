#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "CortexAnimationRebindTestSupport.h"
#include "CortexAnimationTestAnimBlueprint.h"
#include "CortexAnimationTestNativeInstance.h"
#include "CortexAssetMutationGuard.h"
#include "CortexBatchMutation.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "K2Node_CallFunction.h"
#include "Operations/CortexAnimBindingReport.h"
#include "Operations/CortexAnimRebindOps.h"

namespace
{
using namespace CortexAnimTest;

const UFunction* SkeletonFunction(const UAnimBlueprint* AnimBlueprint, const TCHAR* Name)
{
	return AnimBlueprint->SkeletonGeneratedClass != nullptr ? AnimBlueprint->SkeletonGeneratedClass->FindFunctionByName(Name) : nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationRebindFunctionTest,
	"Cortex.Animation.RebindMember.Function",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationRebindFunctionTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);

	if (!TestTrue(TEXT("Blueprint getter"), AddBlueprintFloatGetter(AnimBlueprint, TEXT("GetDir")) != nullptr))
	{
		return false;
	}
	CompileAndAssertClean(*this, AnimBlueprint);

	UAnimationGraph* AnimGraph = FindRootAnimGraph(AnimBlueprint);
	UAnimGraphNode_SequencePlayer* Player = AddSequencePlayerNode(AnimBlueprint);
	UEdGraphNode* GetDirCall = AddCallNode(AnimGraph, SkeletonFunction(AnimBlueprint, TEXT("GetDir")));
	if (!TestTrue(TEXT("fixture nodes"), AnimGraph != nullptr && Player != nullptr && GetDirCall != nullptr))
	{
		return false;
	}
	UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(GetDirCall);
	TestTrue(TEXT("function-typed binding"), BindPropertyByReflection(Player, TEXT("PlayRate"), { TEXT("GetDir") }, /*bFunction=*/true));
	AnimBlueprint->GetPackage()->SetDirtyFlag(false);

	FCortexCommandRouter Router = MakeAnimGraphRouter();
	TestTrue(TEXT("baseline binding path"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetDir") });

	// ── A dry run is exact and clean ──
	{
		const FCortexCommandResult Dry = Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), /*bDryRun=*/true);
		if (TestTrue(*FString::Printf(TEXT("dry run succeeds (%s: %s)"), *Dry.ErrorCode, *Dry.ErrorMessage), Dry.bSuccess) && TestTrue(TEXT("dry run data"), Dry.Data.IsValid()))
		{
			TestEqual(TEXT("one occurrence would be retargeted"), JsonObjectArray(Dry.Data, TEXT("would_retarget")).Num(), 1);
			const TArray<TSharedPtr<FJsonObject>> Unmodified = JsonObjectArray(Dry.Data, TEXT("unmodified_references"));
			if (TestEqual(TEXT("the call node is listed unmodified"), Unmodified.Num(), 1) && Unmodified.Num() == 1)
			{
				TestTrue(TEXT("the unmodified node is the call node"), SameGuid(Unmodified[0]->GetStringField(TEXT("node_guid")), GetDirCall->NodeGuid));
				TestEqual(TEXT("its kind"), Unmodified[0]->GetStringField(TEXT("kind")), FString(TEXT("other_k2_node")));
			}
		}
		TestFalse(TEXT("dry run leaves the package clean"), AnimBlueprint->GetPackage()->IsDirty());
		TestTrue(TEXT("dry run does not touch the binding"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetDir") });
	}

	// ── Apply: one compile, engine virtual agrees, the unrelated K2 call is untouched ──
	{
		FCompileCounter Compiles(AnimBlueprint);
		const FCortexCommandResult Apply = Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), /*bDryRun=*/false);
		if (TestTrue(*FString::Printf(TEXT("apply succeeds (%s: %s)"), *Apply.ErrorCode, *Apply.ErrorMessage), Apply.bSuccess) && TestTrue(TEXT("apply data"), Apply.Data.IsValid()))
		{
			TestEqual(TEXT("one occurrence retargeted"), JsonObjectArray(Apply.Data, TEXT("retargeted")).Num(), 1);
			TestEqual(TEXT("compile_status"), Apply.Data->GetStringField(TEXT("compile_status")), FString(TEXT("success")));
			TestEqual(TEXT("readback_status"), Apply.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
			TestTrue(TEXT("the destination is declared natively"), Apply.Data->GetBoolField(TEXT("declared_in_native")));
			TestEqual(TEXT("the unrelated K2 call is listed unmodified"), JsonObjectArray(Apply.Data, TEXT("unmodified_references")).Num(), 1);
		}
		TestEqual(TEXT("exactly one compile ran"), Compiles.Count, 1);
		TestTrue(TEXT("the binding path follows the native function"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetNativeDirection") });
		TestTrue(TEXT("the engine virtual agrees"), Player->ReferencesFunction(TEXT("GetNativeDirection"), nullptr));
		TestFalse(TEXT("no owned occurrence of the source remains"), Player->ReferencesFunction(TEXT("GetDir"), nullptr));
		TestTrue(TEXT("the K2 call node still names GetDir"), CallNode != nullptr && CallNode->FunctionReference.GetMemberName() == FName(TEXT("GetDir")));
		TestTrue(TEXT("the Anim Blueprint compiled"), AnimBlueprint->Status != BS_Error);
	}

	// ── One editor undo restores the rewritten function binding ──
	{
		const bool bUndone = GEditor != nullptr && GEditor->UndoTransaction();
		TestTrue(TEXT("the editor undo runs"), bUndone);
		TestTrue(TEXT("the function binding is back on the Blueprint getter after the undo"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetDir") });
		TestTrue(TEXT("the engine virtual agrees after the undo"), Player->ReferencesFunction(TEXT("GetDir"), nullptr));
		TestTrue(TEXT("the K2 call node was never touched"), CallNode != nullptr && CallNode->FunctionReference.GetMemberName() == FName(TEXT("GetDir")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationRebindRefusalsTest,
	"Cortex.Animation.RebindMember.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationRebindRefusalsTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);

	if (!TestTrue(TEXT("Blueprint getter and update function"), AddBlueprintFloatGetter(AnimBlueprint, TEXT("GetDir")) != nullptr && AddBlueprintUpdateFunction(AnimBlueprint, TEXT("OnUpdate")) != nullptr))
	{
		return false;
	}
	CompileAndAssertClean(*this, AnimBlueprint);
	UAnimGraphNode_SequencePlayer* Player = AddSequencePlayerNode(AnimBlueprint);
	if (!TestNotNull(TEXT("sequence player"), Player))
	{
		return false;
	}
	TestTrue(TEXT("function-typed binding"), BindPropertyByReflection(Player, TEXT("PlayRate"), { TEXT("GetDir") }, /*bFunction=*/true));
	AnimBlueprint->GetPackage()->SetDirtyFlag(false);
	FCortexCommandRouter Router = MakeAnimGraphRouter();

	// A missing fingerprint.
	{
		TSharedPtr<FJsonObject> Params = RebindParams(AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), false);
		Params->RemoveField(TEXT("expected_fingerprint"));
		const FCortexCommandResult Missing = Router.Execute(TEXT("anim.rebind_function"), Params);
		TestFalse(TEXT("a missing fingerprint is refused"), Missing.bSuccess);
		TestEqual(TEXT("STALE_PRECONDITION"), Missing.ErrorCode, CortexErrorCodes::StalePrecondition);
	}

	// A fingerprint that really is stale: read it, change the asset, then use it.
	{
		const TSharedPtr<FJsonObject> Old = MakeObjectAssetFingerprint(AnimBlueprint).ToJson();
		TestTrue(TEXT("a later edit"), AddVariable(AnimBlueprint, TEXT("LateVariable"), FloatPinType(), TEXT("0.0")));
		AnimBlueprint->GetPackage()->SetDirtyFlag(false);
		TestFalse(TEXT("the fingerprint really changed"), FCortexBatchMutation::FingerprintsMatch(MakeObjectAssetFingerprint(AnimBlueprint).ToJson(), Old));
		TSharedPtr<FJsonObject> Params = RebindParams(AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), false);
		Params->SetObjectField(TEXT("expected_fingerprint"), Old);
		const FCortexCommandResult Stale = Router.Execute(TEXT("anim.rebind_function"), Params);
		TestFalse(TEXT("a stale fingerprint is refused"), Stale.bSuccess);
		TestEqual(TEXT("STALE_PRECONDITION"), Stale.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestTrue(TEXT("the refusal returns the current fingerprint"), Stale.ErrorDetails.IsValid() && Stale.ErrorDetails->HasTypedField<EJson::Object>(TEXT("current_fingerprint")));
	}

	// Unknown names and a mismatched signature.
	{
		TestEqual(TEXT("an unknown from is INVALID_FIELD"), Rebind(Router, AnimBlueprint, TEXT("NoSuchFunction"), TEXT("GetNativeDirection"), false).ErrorCode, CortexErrorCodes::InvalidField);
		TestEqual(TEXT("an unknown to is INVALID_FIELD"), Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("AlsoNoSuchFunction"), false).ErrorCode, CortexErrorCodes::InvalidField);
		TestEqual(TEXT("a mismatched signature is TYPE_MISMATCH"), Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("OnUpdate"), false).ErrorCode, CortexErrorCodes::TypeMismatch);
		TestFalse(TEXT("refusals never dirty the package"), AnimBlueprint->GetPackage()->IsDirty());
		TestTrue(TEXT("refusals never change the binding"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetDir") });
	}

	// A blocked asset path is refused before anything is read or changed.
	{
		FCortexAssetMutationGuard::Block(AnimBlueprint, TEXT("Cortex.Animation.RebindMember.Refusals fixture"));
		FCompileCounter Compiles(AnimBlueprint);
		const FCortexCommandResult Blocked = Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), false);
		TestFalse(TEXT("a blocked path is refused"), Blocked.bSuccess);
		TestEqual(TEXT("INVALID_OPERATION"), Blocked.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(TEXT("the refusal says the path is blocked"), Blocked.ErrorMessage.Contains(TEXT("blocked")));
		TestEqual(TEXT("no compile ran"), Compiles.Count, 0);
		TestFalse(TEXT("nothing was dirtied"), AnimBlueprint->GetPackage()->IsDirty());
		TestTrue(TEXT("nothing was changed"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetDir") });
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationRebindRecoveryTest,
	"Cortex.Animation.RebindMember.Recovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationRebindRecoveryTest::RunTest(const FString& Parameters)
{
	using EPhase = FCortexAnimRebindOps::EPhase;

	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);

	TestTrue(TEXT("Speed variable"), AddVariable(AnimBlueprint, TEXT("Speed"), FloatPinType(), TEXT("0.0")));
	if (!TestTrue(TEXT("Blueprint getter and update function"), AddBlueprintFloatGetter(AnimBlueprint, TEXT("GetDir")) != nullptr && AddBlueprintUpdateFunction(AnimBlueprint, TEXT("OnUpdate")) != nullptr))
	{
		return false;
	}
	CompileAndAssertClean(*this, AnimBlueprint);

	// One node holds a function binding (the source), an unrelated variable binding and a function slot; a K2 call names the source too.
	UAnimationGraph* AnimGraph = FindRootAnimGraph(AnimBlueprint);
	UAnimGraphNode_SequencePlayer* Player = AddSequencePlayerNode(AnimBlueprint);
	UEdGraphNode* CallNode = AddCallNode(AnimGraph, SkeletonFunction(AnimBlueprint, TEXT("GetDir")));
	if (!TestTrue(TEXT("fixture nodes"), Player != nullptr && CallNode != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("source binding"), BindPropertyByReflection(Player, TEXT("PlayRate"), { TEXT("GetDir") }, /*bFunction=*/true));
	TestTrue(TEXT("unrelated variable binding"), BindPropertyByReflection(Player, TEXT("StartPosition"), { TEXT("Speed") }));
	Player->UpdateFunction.SetSelfMember(TEXT("OnUpdate"));
	AnimBlueprint->GetPackage()->SetDirtyFlag(false);

	FCortexCommandRouter Router = MakeAnimGraphRouter();
	auto ExpectRestored = [&](const TCHAR* Label)
	{
		const FString Prefix(Label);
		TestTrue(Prefix + TEXT(": the source binding is back"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetDir") });
		TestTrue(Prefix + TEXT(": the unrelated binding is intact"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("StartPosition")) == TArray<FString>{ TEXT("Speed") });
		FString Function;
		FString DeclaredIn;
		TestTrue(Prefix + TEXT(": the slot is reported"), ReportedSlot(Router, AnimBlueprint, Player->NodeGuid, TEXT("update"), Function, DeclaredIn));
		TestEqual(Prefix + TEXT(": the slot is back"), Function, FString(TEXT("OnUpdate")));
		TestTrue(Prefix + TEXT(": the K2 call is intact"), Cast<UK2Node_CallFunction>(CallNode)->FunctionReference.GetMemberName() == FName(TEXT("GetDir")));
		TestFalse(Prefix + TEXT(": the dirty flag is restored"), AnimBlueprint->GetPackage()->IsDirty());
	};

	struct FCase
	{
		const TCHAR* Label;
		EPhase Phase;
		bool bCompile;
		bool bSave;
		FString ExpectedCode;
		int32 ExpectedCompiles;
	};
	const FCase Cases[] = {
		{ TEXT("after the edits"), EPhase::AfterMutation, true, false, CortexErrorCodes::VerificationFailed, 1 },
		{ TEXT("compile"), EPhase::Compile, true, false, CortexErrorCodes::CompileFailed, 1 },
		{ TEXT("readback"), EPhase::Readback, true, false, CortexErrorCodes::VerificationFailed, 2 },
		{ TEXT("readback without compile"), EPhase::Readback, false, false, CortexErrorCodes::VerificationFailed, 1 },
		{ TEXT("save"), EPhase::Save, true, true, CortexErrorCodes::SaveFailed, 2 },
	};
	for (const FCase& Case : Cases)
	{
		FCortexAnimRebindOps::SetFaultHookForTest([Phase = Case.Phase](EPhase Called, UAnimBlueprint*) { return Called == Phase; });
		FCompileCounter Compiles(AnimBlueprint);
		const FCortexCommandResult Failed = Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), false, Case.bCompile, Case.bSave);
		FCortexAnimRebindOps::ClearFaultHookForTest();

		TestFalse(FString(Case.Label) + TEXT(": the call fails"), Failed.bSuccess);
		TestEqual(FString(Case.Label) + TEXT(": the error code"), Failed.ErrorCode, Case.ExpectedCode);
		FString RollbackStatus;
		FString RecoveryCompile;
		if (Failed.ErrorDetails.IsValid())
		{
			Failed.ErrorDetails->TryGetStringField(TEXT("rollback_status"), RollbackStatus);
			Failed.ErrorDetails->TryGetStringField(TEXT("recovery_compile"), RecoveryCompile);
		}
		TestEqual(FString(Case.Label) + TEXT(": rollback_status"), RollbackStatus, FString(TEXT("restored")));
		TestEqual(FString(Case.Label) + TEXT(": a recovery compile ran"), RecoveryCompile, FString(TEXT("run")));
		TestEqual(FString(Case.Label) + TEXT(": compile count (normal compile plus exactly one recovery compile)"), Compiles.Count, Case.ExpectedCompiles);
		ExpectRestored(Case.Label);
		TestTrue(FString(Case.Label) + TEXT(": the asset is not blocked"), Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), true).bSuccess);
	}

	// A restore step that loses content: the slot comes back, but a binding the journal captured is gone. The verification must catch it,
	// keep the package dirty and block the asset.
	{
		FCortexAnimRebindOps::SetFaultHookForTest([Player](EPhase Called, UAnimBlueprint*)
		{
			if (Called == EPhase::Readback)
			{
				return true;
			}
			if (Called == EPhase::AfterRestore)
			{
				RemoveBindingByReflection(Player, TEXT("StartPosition"));
			}
			return false;
		});
		const FCortexCommandResult Failed = Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), false);
		FCortexAnimRebindOps::ClearFaultHookForTest();

		TestFalse(TEXT("lost content: the call fails"), Failed.bSuccess);
		TestEqual(TEXT("lost content: VERIFICATION_FAILED"), Failed.ErrorCode, CortexErrorCodes::VerificationFailed);
		FString RollbackStatus;
		if (Failed.ErrorDetails.IsValid())
		{
			Failed.ErrorDetails->TryGetStringField(TEXT("rollback_status"), RollbackStatus);
		}
		TestEqual(TEXT("lost content: rollback_status"), RollbackStatus, FString(TEXT("verification_failed")));
		TestTrue(TEXT("lost content: the package stays dirty (no unproven clean flag)"), AnimBlueprint->GetPackage()->IsDirty());
		const FCortexCommandResult Blocked = Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), true);
		TestFalse(TEXT("lost content: the asset path is blocked"), Blocked.bSuccess);
		TestEqual(TEXT("lost content: INVALID_OPERATION"), Blocked.ErrorCode, CortexErrorCodes::InvalidOperation);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationRebindSlotTest,
	"Cortex.Animation.RebindMember.Slot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationRebindSlotTest::RunTest(const FString& Parameters)
{
	using EPhase = FCortexAnimRebindOps::EPhase;

	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);

	TestTrue(TEXT("Speed variable"), AddVariable(AnimBlueprint, TEXT("Speed"), FloatPinType(), TEXT("0.0")));
	if (!TestNotNull(TEXT("thread-safe update function with the slot prototype signature"), AddBlueprintUpdateFunction(AnimBlueprint, TEXT("OnUpdate"))))
	{
		return false;
	}
	UAnimGraphNode_SequencePlayer* Player = AddSequencePlayerNode(AnimBlueprint);
	if (!TestTrue(TEXT("a sequence player the compiler consumes"), Player != nullptr && ConnectToResult(AnimBlueprint, Player)))
	{
		return false;
	}
	TestTrue(TEXT("variable binding"), BindPropertyByReflection(Player, TEXT("PlayRate"), { TEXT("Speed") }));
	Player->UpdateFunction.SetSelfMember(TEXT("OnUpdate"));
	CompileAndAssertClean(*this, AnimBlueprint);
	AnimBlueprint->GetPackage()->SetDirtyFlag(false);

	const UFunction* Hook = UCortexAnimTestNativeInstance::StaticClass()->FindFunctionByName(TEXT("NativeUpdateHook"));
	const UFunction* Update = SkeletonFunction(AnimBlueprint, TEXT("OnUpdate"));
	if (!TestTrue(TEXT("both slot functions exist"), Hook != nullptr && Update != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("the Blueprint function and the native hook are signature-compatible"), Update->IsSignatureCompatibleWith(Hook));

	FCortexCommandRouter Router = MakeAnimGraphRouter();

	// ── The slot rolls back to a compile-valid state ──
	{
		FCortexAnimRebindOps::SetFaultHookForTest([](EPhase Called, UAnimBlueprint*) { return Called == EPhase::Readback; });
		const FCortexCommandResult Failed = Rebind(Router, AnimBlueprint, TEXT("OnUpdate"), TEXT("NativeUpdateHook"), false);
		FCortexAnimRebindOps::ClearFaultHookForTest();
		TestFalse(TEXT("the injected readback failure fails the call"), Failed.bSuccess);
		FString Function;
		FString DeclaredIn;
		TestTrue(TEXT("the slot is reported after the rollback"), ReportedSlot(Router, AnimBlueprint, Player->NodeGuid, TEXT("update"), Function, DeclaredIn));
		TestEqual(TEXT("the slot is back on the Blueprint function"), Function, FString(TEXT("OnUpdate")));
		TestEqual(TEXT("declared_in blueprint"), DeclaredIn, FString(TEXT("blueprint")));
		TestTrue(TEXT("the variable binding is untouched"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("Speed") });
		TestTrue(TEXT("the recovered Anim Blueprint compiles clean"), AnimBlueprint->Status == BS_UpToDate);
	}

	// ── The slot migrates to the native hook and the compiler accepts it on the consumed node ──
	{
		const FCortexCommandResult Apply = Rebind(Router, AnimBlueprint, TEXT("OnUpdate"), TEXT("NativeUpdateHook"), false);
		if (TestTrue(*FString::Printf(TEXT("the slot retarget succeeds (%s: %s)"), *Apply.ErrorCode, *Apply.ErrorMessage), Apply.bSuccess) && Apply.Data.IsValid())
		{
			TestEqual(TEXT("one slot occurrence retargeted"), JsonObjectArray(Apply.Data, TEXT("retargeted")).Num(), 1);
			TestEqual(TEXT("compile_status"), Apply.Data->GetStringField(TEXT("compile_status")), FString(TEXT("success")));
		}
		FString Function;
		FString DeclaredIn;
		TestTrue(TEXT("the slot is reported"), ReportedSlot(Router, AnimBlueprint, Player->NodeGuid, TEXT("update"), Function, DeclaredIn));
		TestEqual(TEXT("the slot names the native hook"), Function, FString(TEXT("NativeUpdateHook")));
		TestEqual(TEXT("declared_in native"), DeclaredIn, FString(TEXT("native")));
		TestTrue(TEXT("the compiled class resolves the slot natively"), Player->UpdateFunction.ResolveMember<UFunction>(AnimBlueprint->GeneratedClass) != nullptr);
		TestTrue(TEXT("the Anim Blueprint compiles clean with the consumed node"), AnimBlueprint->Status == BS_UpToDate);
	}

	// ── One editor undo restores the slot ──
	{
		const bool bUndone = GEditor != nullptr && GEditor->UndoTransaction();
		TestTrue(TEXT("the editor undo runs"), bUndone);
		TestTrue(TEXT("the slot is back after the undo"), Player->UpdateFunction.GetMemberName() == FName(TEXT("OnUpdate")));
		TestTrue(TEXT("the variable binding survived the undo"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("Speed") });
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationRebindExactEditsTest,
	"Cortex.Animation.RebindMember.ExactEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationRebindExactEditsTest::RunTest(const FString& Parameters)
{
	using EPhase = FCortexAnimRebindOps::EPhase;

	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);

	TestTrue(TEXT("Speed variable"), AddVariable(AnimBlueprint, TEXT("Speed"), FloatPinType(), TEXT("0.0")));
	if (!TestNotNull(TEXT("Blueprint getter"), AddBlueprintFloatGetter(AnimBlueprint, TEXT("GetDir"))))
	{
		return false;
	}
	CompileAndAssertClean(*this, AnimBlueprint);
	UAnimGraphNode_SequencePlayer* Player = AddSequencePlayerNode(AnimBlueprint);
	if (!TestNotNull(TEXT("sequence player"), Player))
	{
		return false;
	}
	// A planned entry (PlayRate -> GetDir) next to an unplanned one (StartPosition -> Speed) on the same node.
	TestTrue(TEXT("source binding"), BindPropertyByReflection(Player, TEXT("PlayRate"), { TEXT("GetDir") }, /*bFunction=*/true));
	TestTrue(TEXT("unplanned binding"), BindPropertyByReflection(Player, TEXT("StartPosition"), { TEXT("Speed") }));
	AnimBlueprint->GetPackage()->SetDirtyFlag(false);
	FCortexCommandRouter Router = MakeAnimGraphRouter();

	const FString OwnedTextBefore = ReadBindingField(Player, TEXT("PlayRate"), TEXT("PathAsText"));
	const FString ContextBefore = ReadBindingField(Player, TEXT("PlayRate"), TEXT("ContextId"));
	const FString FlagBefore = ReadBindingField(Player, TEXT("StartPosition"), TEXT("bOnlyUpdateWhenActive"));

	struct FTamper
	{
		const TCHAR* Label;
		FName Key;
		const TCHAR* Field;
		const TCHAR* Value;
	};
	// Each fault changes a field the plan does not authorize, between the edits and their verification: an unplanned entry's flag, the owned
	// entry's display text (which only the engine's own text for the new path may produce), and a field no projection ever looked at.
	const FTamper Tampers[] = {
		{ TEXT("an unplanned entry's flag"), TEXT("StartPosition"), TEXT("bOnlyUpdateWhenActive"), TEXT("True") },
		{ TEXT("the owned entry's display text"), TEXT("PlayRate"), TEXT("PathAsText"), TEXT("INVTEXT(\"tampered\")") },
		{ TEXT("a field the projection never compared"), TEXT("PlayRate"), TEXT("ContextId"), TEXT("Tampered") },
	};
	for (const FTamper& Tamper : Tampers)
	{
		FCortexAnimRebindOps::SetFaultHookForTest([&](EPhase Called, UAnimBlueprint*)
		{
			if (Called == EPhase::AfterMutation)
			{
				TestTrue(FString(Tamper.Label) + TEXT(": the fault is injected"), TamperBindingField(Player, Tamper.Key, Tamper.Field, Tamper.Value));
			}
			return false;
		});
		const FCortexCommandResult Failed = Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), false);
		FCortexAnimRebindOps::ClearFaultHookForTest();

		TestFalse(FString(Tamper.Label) + TEXT(": the exact edit verification fails the call"), Failed.bSuccess);
		TestEqual(FString(Tamper.Label) + TEXT(": the error code"), Failed.ErrorCode, CortexErrorCodes::VerificationFailed);
		FString Phase;
		FString RollbackStatus;
		if (Failed.ErrorDetails.IsValid())
		{
			Failed.ErrorDetails->TryGetStringField(TEXT("failed_phase"), Phase);
			Failed.ErrorDetails->TryGetStringField(TEXT("rollback_status"), RollbackStatus);
		}
		TestEqual(FString(Tamper.Label) + TEXT(": caught by the readback"), Phase, FString(TEXT("readback")));
		TestEqual(FString(Tamper.Label) + TEXT(": rollback_status"), RollbackStatus, FString(TEXT("restored")));
		TestTrue(FString(Tamper.Label) + TEXT(": the source binding is back"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetDir") });
		TestEqual(FString(Tamper.Label) + TEXT(": the owned display text is back"), ReadBindingField(Player, TEXT("PlayRate"), TEXT("PathAsText")), OwnedTextBefore);
		TestEqual(FString(Tamper.Label) + TEXT(": the context is back"), ReadBindingField(Player, TEXT("PlayRate"), TEXT("ContextId")), ContextBefore);
		TestEqual(FString(Tamper.Label) + TEXT(": the unplanned flag is back"), ReadBindingField(Player, TEXT("StartPosition"), TEXT("bOnlyUpdateWhenActive")), FlagBefore);
		TestFalse(FString(Tamper.Label) + TEXT(": the dirty flag is restored"), AnimBlueprint->GetPackage()->IsDirty());
	}

	// The same faults injected after the exact edit check, while the compile proceeds normally: only the final readback, which runs
	// after the compile and before any save, can catch them.
	const FTamper LateTampers[] = {
		{ TEXT("a field the projection never compared, around the compile"), TEXT("PlayRate"), TEXT("ContextId"), TEXT("Tampered") },
		{ TEXT("an unplanned entry's context, around the compile"), TEXT("StartPosition"), TEXT("ContextId"), TEXT("Tampered") },
	};
	for (const FTamper& Tamper : LateTampers)
	{
		FCortexAnimRebindOps::SetFaultHookForTest([&](EPhase Called, UAnimBlueprint*)
		{
			if (Called == EPhase::Compile || Called == EPhase::AfterCompile)
			{
				TestTrue(FString(Tamper.Label) + TEXT(": the fault is injected"), TamperBindingField(Player, Tamper.Key, Tamper.Field, Tamper.Value));
			}
			return false;
		});
		FCompileCounter Compiles(AnimBlueprint);
		const FCortexCommandResult Failed = Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), false);
		FCortexAnimRebindOps::ClearFaultHookForTest();

		TestFalse(FString(Tamper.Label) + TEXT(": the final readback fails the call"), Failed.bSuccess);
		TestEqual(FString(Tamper.Label) + TEXT(": the error code"), Failed.ErrorCode, CortexErrorCodes::VerificationFailed);
		FString Phase;
		FString RollbackStatus;
		if (Failed.ErrorDetails.IsValid())
		{
			Failed.ErrorDetails->TryGetStringField(TEXT("failed_phase"), Phase);
			Failed.ErrorDetails->TryGetStringField(TEXT("rollback_status"), RollbackStatus);
		}
		TestEqual(FString(Tamper.Label) + TEXT(": caught by the readback"), Phase, FString(TEXT("readback")));
		TestEqual(FString(Tamper.Label) + TEXT(": rollback_status"), RollbackStatus, FString(TEXT("restored")));
		TestEqual(FString(Tamper.Label) + TEXT(": the normal compile and exactly one recovery compile ran"), Compiles.Count, 2);
		TestEqual(FString(Tamper.Label) + TEXT(": the owned context is back"), ReadBindingField(Player, TEXT("PlayRate"), TEXT("ContextId")), ContextBefore);
		TestTrue(FString(Tamper.Label) + TEXT(": the source binding is back"), ReportedBindingPath(Router, AnimBlueprint, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetDir") });
		TestFalse(FString(Tamper.Label) + TEXT(": the dirty flag is restored"), AnimBlueprint->GetPackage()->IsDirty());
	}

	// Without a fault the same call passes the same verification, and the owned entry carries the engine's text for the new path.
	const FCortexCommandResult Apply = Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), false);
	TestTrue(*FString::Printf(TEXT("the unfaulted apply succeeds (%s: %s)"), *Apply.ErrorCode, *Apply.ErrorMessage), Apply.bSuccess);
	FString ExpectedText;
	FTextStringHelper::WriteToBuffer(ExpectedText, FCortexAnimBindingReport::MakePathText(AnimBlueprint, { TEXT("GetNativeDirection") }));
	TestEqual(TEXT("the owned display text is the engine's text of the new path"), ReadBindingField(Player, TEXT("PlayRate"), TEXT("PathAsText")), ExpectedText);
	TestEqual(TEXT("the unplanned entry kept its flag"), ReadBindingField(Player, TEXT("StartPosition"), TEXT("bOnlyUpdateWhenActive")), FlagBefore);

	return true;
}
