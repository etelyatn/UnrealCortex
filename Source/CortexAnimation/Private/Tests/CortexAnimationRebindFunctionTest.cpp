#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "CortexAnimationTestAnimBlueprint.h"
#include "CortexAssetFingerprint.h"
#include "CortexAnimationTestNativeInstance.h"
#include "CortexGraphMemberReferences.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Operations/CortexAnimBindingReflection.h"
#include "Operations/CortexAnimRebindOps.h"

namespace
{
using namespace CortexAnimTest;

/** Exactly the fingerprint the command compares against (core.asset_fingerprint shape). */
TSharedPtr<FJsonObject> Fingerprint(const UObject* Asset)
{
	return MakeObjectAssetFingerprint(Asset).ToJson();
}

/** Params with a fingerprint read at call time (the fingerprint tracks the package's saved state). */
TSharedPtr<FJsonObject> RebindParams(const UAnimBlueprint* AnimBlueprint, const TCHAR* From, const TCHAR* To, const bool bDryRun, const bool bCompile)
{
	TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
	Params->SetStringField(TEXT("from"), From);
	Params->SetStringField(TEXT("to"), To);
	Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint(AnimBlueprint));
	Params->SetBoolField(TEXT("dry_run"), bDryRun);
	Params->SetBoolField(TEXT("compile"), bCompile);
	return Params;
}

FCortexCommandResult Info(FCortexCommandRouter& Router, const UAnimBlueprint* AnimBlueprint)
{
	TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
	Params->SetArrayField(TEXT("include"), { MakeShared<FJsonValueString>(TEXT("bindings")), MakeShared<FJsonValueString>(TEXT("node_functions")) });
	return Router.Execute(TEXT("anim.get_animbp_info"), Params);
}

/** The reported path of a node's binding for one pin property. */
TArray<FString> BindingPath(const TSharedPtr<FJsonObject>& Data, const FGuid& NodeGuid, const TCHAR* PropertyName)
{
	for (const TSharedPtr<FJsonObject>& Entry : CollectionItems(Data, TEXT("bindings")))
	{
		FString NodeGuidText;
		FString Property;
		if (Entry->TryGetStringField(TEXT("node_guid"), NodeGuidText) && SameGuid(NodeGuidText, NodeGuid)
			&& Entry->TryGetStringField(TEXT("property_name"), Property) && Property == PropertyName)
		{
			TArray<FString> Path;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Entry->TryGetArrayField(TEXT("path"), Values) && Values != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					FString Segment;
					if (Value.IsValid() && Value->TryGetString(Segment))
					{
						Path.Add(Segment);
					}
				}
			}
			return Path;
		}
	}
	return {};
}

TArray<FString> RetargetedForms(const UBlueprint* AnimBlueprint, const TCHAR* Function)
{
	TArray<FString> Forms;
	TArray<FCortexMemberReference> References;
	FCortexGraphMemberReferences::FindFunctionReferences(AnimBlueprint, Function, References);
	for (const FCortexMemberReference& Reference : References)
	{
		// Retargetable kinds only: a K2 call node is a reference the command leaves alone (returned as unmodified).
		if (Reference.Kind == TEXT("anim_node_binding") || Reference.Kind == TEXT("anim_node_function"))
		{
			Forms.Add(Reference.Kind + TEXT(":") + Reference.Slot);
		}
	}
	Forms.Sort();
	return Forms;
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

	// GetDir: a Blueprint getter (no parameters, float return) a function binding path can name; OnUpdate: a void Blueprint
	// function for a function slot. The engine requires exactly one parameter (the return) for a function path
	// (PropertyAccessEditor.cpp:169-181) and prototype compatibility for a slot (AnimGraphNode_Base.cpp:273-277), so the binding
	// and the slot are retargeted with two separate pairs.
	if (!TestTrue(TEXT("Blueprint getter"), AddBlueprintFloatGetter(AnimBlueprint, TEXT("GetDir")) != nullptr)
		|| !TestTrue(TEXT("Blueprint slot function"), AddBlueprintFunction(AnimBlueprint, TEXT("OnUpdate")) != nullptr))
	{
		return false;
	}
	CompileAndAssertClean(*this, AnimBlueprint);

	UAnimationGraph* AnimGraph = FindRootAnimGraph(AnimBlueprint);
	UAnimGraphNode_SequencePlayer* Player = AddSequencePlayerNode(AnimBlueprint);
	const UFunction* GetDir = AnimBlueprint->SkeletonGeneratedClass->FindFunctionByName(TEXT("GetDir"));
	UEdGraphNode* GetDirCall = AddCallNode(AnimGraph, GetDir);
	if (!TestTrue(TEXT("fixture nodes"), AnimGraph != nullptr && Player != nullptr && GetDir != nullptr && GetDirCall != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("function-typed binding"), BindPropertyByReflection(Player, TEXT("PlayRate"), { TEXT("GetDir") }, /*bFunction=*/true));
	Player->UpdateFunction.SetSelfMember(TEXT("OnUpdate"));

	FCortexCommandRouter Router = MakeAnimGraphRouter();
	const FCortexCommandResult Before = Info(Router, AnimBlueprint);
	if (!TestTrue(TEXT("baseline info"), Before.bSuccess) || !TestTrue(TEXT("baseline data"), Before.Data.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("baseline binding path"), BindingPath(Before.Data, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetDir") });
	const TArray<FString> BaselineToForms = RetargetedForms(AnimBlueprint, TEXT("GetNativeDirection"));
	const int32 BaselineFromCount = RetargetedForms(AnimBlueprint, TEXT("GetDir")).Num();
	TestEqual(TEXT("one retargetable reference to GetDir"), BaselineFromCount, 1);

	AnimBlueprint->GetPackage()->SetDirtyFlag(false);

	// ── Preconditions ──
	{
		const FCortexCommandResult NoFingerprint = Router.Execute(TEXT("anim.rebind_function"),
			[&] { TSharedPtr<FJsonObject> P = RebindParams(AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), false, true); P->RemoveField(TEXT("expected_fingerprint")); return P; }());
		TestFalse(TEXT("a missing fingerprint is refused"), NoFingerprint.bSuccess);
		TestEqual(TEXT("STALE_PRECONDITION"), NoFingerprint.ErrorCode, CortexErrorCodes::StalePrecondition);

		const FCortexCommandResult UnknownFrom = Router.Execute(TEXT("anim.rebind_function"),
			RebindParams(AnimBlueprint, TEXT("NoSuchFunction"), TEXT("GetNativeDirection"), false, true));
		TestFalse(TEXT("an unknown from is refused"), UnknownFrom.bSuccess);
		TestEqual(TEXT("unknown from code"), UnknownFrom.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult UnknownTo = Router.Execute(TEXT("anim.rebind_function"),
			RebindParams(AnimBlueprint, TEXT("GetDir"), TEXT("AlsoNoSuchFunction"), false, true));
		TestFalse(TEXT("an unknown to is refused"), UnknownTo.bSuccess);

		const FCortexCommandResult Mismatch = Router.Execute(TEXT("anim.rebind_function"),
			RebindParams(AnimBlueprint, TEXT("GetDir"), TEXT("OnUpdate"), false, true));
		TestFalse(TEXT("a signature mismatch is refused"), Mismatch.bSuccess);
		TestEqual(TEXT("TYPE_MISMATCH"), Mismatch.ErrorCode, CortexErrorCodes::TypeMismatch);
		TestFalse(TEXT("refusals never dirty the package"), AnimBlueprint->GetPackage()->IsDirty());
	}

	// ── Dry run ──
	{
		const FCortexCommandResult Dry = Router.Execute(TEXT("anim.rebind_function"),
			RebindParams(AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), /*bDryRun=*/true, /*bCompile=*/true));
		if (TestTrue(*FString::Printf(TEXT("dry run succeeds (%s: %s)"), *Dry.ErrorCode, *Dry.ErrorMessage), Dry.bSuccess) && TestTrue(TEXT("dry run data"), Dry.Data.IsValid()))
		{
			TestEqual(TEXT("one node would be retargeted"), JsonObjectArray(Dry.Data, TEXT("would_retarget")).Num(), 1);
			const TArray<TSharedPtr<FJsonObject>> Unmodified = JsonObjectArray(Dry.Data, TEXT("unmodified_references"));
			TestEqual(TEXT("the call node is listed unmodified"), Unmodified.Num(), 1);
			if (Unmodified.Num() == 1)
			{
				TestTrue(TEXT("the unmodified node is the call node"), SameGuid(Unmodified[0]->GetStringField(TEXT("node_guid")), GetDirCall->NodeGuid));
				TestEqual(TEXT("its kind"), Unmodified[0]->GetStringField(TEXT("kind")), FString(TEXT("other_k2_node")));
			}
			TestFalse(TEXT("dry run leaves the package clean"), AnimBlueprint->GetPackage()->IsDirty());
			TestTrue(TEXT("dry run does not touch the binding"), BindingPath(Info(Router, AnimBlueprint).Data, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetDir") });
		}
	}

	// ── Injected compile failure restores the journal (content, not the fingerprint) ──
	{
		const TArray<FString> FromBefore = RetargetedForms(AnimBlueprint, TEXT("GetDir"));
		AnimBlueprint->GetPackage()->SetDirtyFlag(false);
		FCortexAnimRebindOps::SetCompileFailureHookForTest([](UAnimBlueprint*) { return true; });
		const FCortexCommandResult Failed = Router.Execute(TEXT("anim.rebind_function"),
			RebindParams(AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), /*bDryRun=*/false, /*bCompile=*/true));
		FCortexAnimRebindOps::ClearCompileFailureHookForTest();

		TestFalse(TEXT("an injected compile failure fails the call"), Failed.bSuccess);
		TestEqual(TEXT("COMPILE_FAILED"), Failed.ErrorCode, CortexErrorCodes::CompileFailed);
		FString RollbackStatus;
		if (Failed.ErrorDetails.IsValid())
		{
			Failed.ErrorDetails->TryGetStringField(TEXT("rollback_status"), RollbackStatus);
		}
		TestEqual(TEXT("rollback_status"), RollbackStatus, FString(TEXT("restored")));
		TestTrue(TEXT("the binding path is restored by content"), BindingPath(Info(Router, AnimBlueprint).Data, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetDir") });
		TestTrue(TEXT("the reference set is restored"), RetargetedForms(AnimBlueprint, TEXT("GetDir")) == FromBefore);
		TestFalse(TEXT("the dirty flag is restored"), AnimBlueprint->GetPackage()->IsDirty());
	}

	// ── Apply (compile once) ──
	{
		const FCortexCommandResult Apply = Router.Execute(TEXT("anim.rebind_function"),
			RebindParams(AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), /*bDryRun=*/false, /*bCompile=*/true));
		if (TestTrue(*FString::Printf(TEXT("apply succeeds (%s: %s)"), *Apply.ErrorCode, *Apply.ErrorMessage), Apply.bSuccess) && TestTrue(TEXT("apply data"), Apply.Data.IsValid()))
		{
			TestEqual(TEXT("one node retargeted"), JsonObjectArray(Apply.Data, TEXT("retargeted")).Num(), 1);
			TestEqual(TEXT("compile_status"), Apply.Data->GetStringField(TEXT("compile_status")), FString(TEXT("success")));
			TestEqual(TEXT("readback_status"), Apply.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		}
		const FCortexCommandResult After = Info(Router, AnimBlueprint);
		TestTrue(TEXT("the binding path follows the native function"), BindingPath(After.Data, Player->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetNativeDirection") });
		TestEqual(TEXT("no retargetable reference to GetDir remains"), RetargetedForms(AnimBlueprint, TEXT("GetDir")).Num(), 0);
		TestEqual(TEXT("the retargeted reference now names GetNativeDirection"), RetargetedForms(AnimBlueprint, TEXT("GetNativeDirection")).Num(), BaselineToForms.Num() + 1);
		TestTrue(TEXT("the engine virtual agrees"), Player->ReferencesFunction(TEXT("GetNativeDirection"), nullptr));
		TestTrue(TEXT("the Anim Blueprint compiled"), AnimBlueprint->Status != BS_Error);
	}

	// ── Slot retarget (compile disabled: the fixture slot function is not prototype-compatible) ──
	{
		const FCortexCommandResult Slot = Router.Execute(TEXT("anim.rebind_function"),
			RebindParams(AnimBlueprint, TEXT("OnUpdate"), TEXT("NativeUpdateHook"), /*bDryRun=*/false, /*bCompile=*/false));
		if (TestTrue(TEXT("slot retarget succeeds"), Slot.bSuccess) && TestTrue(TEXT("slot data"), Slot.Data.IsValid()))
		{
			TestEqual(TEXT("compile not run"), Slot.Data->GetStringField(TEXT("compile_status")), FString(TEXT("not_run")));
		}
		const TSharedPtr<FJsonObject> After = Info(Router, AnimBlueprint).Data;
		bool bSlotFollows = false;
		for (const TSharedPtr<FJsonObject>& Entry : CollectionItems(After, TEXT("node_functions")))
		{
			FString NodeGuidText;
			if (Entry->TryGetStringField(TEXT("node_guid"), NodeGuidText) && SameGuid(NodeGuidText, Player->NodeGuid)
				&& Entry->GetStringField(TEXT("slot")) == TEXT("update"))
			{
				bSlotFollows = Entry->GetStringField(TEXT("function_name")) == TEXT("NativeUpdateHook");
			}
		}
		TestTrue(TEXT("the slot names the native function"), bSlotFollows);
		TestEqual(TEXT("the slot reference is retargeted"), RetargetedForms(AnimBlueprint, TEXT("OnUpdate")).Num(), 0);
	}

	return true;
}
