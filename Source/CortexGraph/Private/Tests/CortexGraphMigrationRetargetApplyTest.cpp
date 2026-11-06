// retarget_calls apply, exact-echo approval and journaled recovery (#122 Task 11). A message, typed call or cast of the old owner is
// re-pointed at the new one (a message becomes a direct typed call when mode is call), the reviewed links are re-made through the
// schema, and a failure at any phase restores every touched node's pins and links with one recovery compile.

#include "Misc/AutomationTest.h"

#include "CortexGraphRetargetTestSupport.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace
{
	using namespace CortexGraphRetargetTest;

	TSharedPtr<FJsonObject> CloneObject(const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
		Clone->Values = Source->Values;
		return Clone;
	}

	/** Executes the apply request; the outcome and the error are returned for the caller to assert on. */
	bool RunApply(UBlueprint* Blueprint, UBlueprint* Interface, const TCHAR* Mode, FCortexGraphPatchOutcome& OutOutcome, FCortexCommandResult& OutError)
	{
		FCortexGraphPreparedPatch Prepared;
		if (!Preview(Blueprint, Interface, ValuePinMap(), Mode, Prepared, OutError))
		{
			return false;
		}
		return FCortexGraphPatchOps::Execute(Blueprint, ApplyRequest(Blueprint, Interface, ValuePinMap(), Mode, Prepared), OutOutcome, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationRetargetApplyTest,
	"Cortex.Graph.Authoring.Migration.Retarget.Apply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetargetApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphRetargetTest;

	FScope Scope;
	UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122RetargetApply"));
	FCaller Caller;
	if (!TestNotNull(TEXT("BPI"), Interface) || !TestTrue(TEXT("Caller"), MakeCaller(Scope, Interface, TEXT("BP_Issue122RetargetApply"), Caller)))
	{
		return false;
	}
	UClass* const Native = UCortexIssue122GraphInterface::StaticClass();
	const FGuid EventMessageGuid = Caller.EventMessage->NodeGuid;
	const FGuid HelperMessageGuid = Caller.HelperMessage->NodeGuid;
	const FGuid NestedMessageGuid = Caller.NestedMessage->NodeGuid;
	const FGuid CastGuid = Caller.Cast->NodeGuid;
	const FGuid TypedGuid = Caller.Typed->NodeGuid;
	const FString UnrelatedBefore = NodeDigest(Caller.Unrelated);
	const FGuid ConsumerGuid = Caller.Consumer->NodeGuid;
	const FGuid SelfGuid = Caller.Self->NodeGuid;
	const FGuid BeginPlayGuid = Caller.BeginPlay->NodeGuid;

	// message -> message, typed_call -> typed_call and interface_cast (recreated with its output pin mapped), with the exact echo.
	{
		FCompileCounter Counter;
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		const bool bApplied = RunApply(Caller.Blueprint, Interface, nullptr, Outcome, Error);
		TestTrue(FString::Printf(TEXT("The apply with the exact echo succeeds [%s: %s] [%s]"), *Error.ErrorCode, *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))), bApplied);
		if (!bApplied)
		{
			return false;
		}
		TestEqual(TEXT("Apply status"), Outcome.ApplyStatus, FString(TEXT("applied")));
		TestEqual(TEXT("Compile status"), Outcome.CompileStatus, FString(TEXT("compiled")));
		TestEqual(TEXT("Readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
		TestEqual(TEXT("Exactly one target compile"), Counter.TargetCompiles, 1);
		TestEqual(TEXT("No recovery compile"), Counter.RecoveryCompiles, 0);
		TestEqual(TEXT("The outcome reports the one target compile"), Outcome.TargetCompileCount, 1);
		TestTrue(TEXT("The apply publishes the retarget inventory"), Outcome.RetargetInventory.IsValid());
		if (Outcome.RetargetInventory.IsValid())
		{
			TestFalse(TEXT("... no longer awaiting approval"), Outcome.RetargetInventory->GetBoolField(TEXT("awaiting_approval")));
			TestEqual(TEXT("... with every reviewed site"), static_cast<int32>(Outcome.RetargetInventory->GetNumberField(TEXT("site_count"))), 5);
		}
		TestTrue(TEXT("The asset compiles"), Caller.Blueprint->Status == BS_UpToDate);

		UEdGraph* const Events = Caller.EventGraph;
		UEdGraphNode* const EventMessage = FCortexGraphMigrationOps::FindNodeByGuidInGraph(Events, EventMessageGuid);
		UEdGraphNode* const HelperMessage = FCortexGraphMigrationOps::FindNodeByGuidInGraph(Caller.HelperGraph, HelperMessageGuid);
		UEdGraphNode* const NestedMessage = FCortexGraphMigrationOps::FindNodeByGuidInGraph(Caller.NestedGraph, NestedMessageGuid);
		const UK2Node_Message* const Messages[] = { Cast<UK2Node_Message>(EventMessage), Cast<UK2Node_Message>(HelperMessage), Cast<UK2Node_Message>(NestedMessage) };
		for (const UK2Node_Message* Message : Messages)
		{
			if (!TestNotNull(TEXT("A message node keeps its GUID and stays a message node"), Message))
			{
				continue;
			}
			TestTrue(TEXT("... its reference now names the native interface"),
				Message->FunctionReference.GetMemberParentClass(Message->GetBlueprintClassFromNode())->GetAuthoritativeClass() == Native);
			TestEqual(TEXT("... and the function"), Message->FunctionReference.GetMemberName(), FName(TEXT("GetValue")));
			TestNotNull(TEXT("... its output now carries the new pin name"), Message->FindPin(TEXT("ReturnValue"), EGPD_Output));
			TestNull(TEXT("... and the renamed pin is gone"), Message->FindPin(TEXT("Value"), EGPD_Output));
			TestNotNull(TEXT("... the target still resolves to a function"), Message->GetTargetFunction());
		}
		TestTrue(TEXT("The event message keeps its links: BeginPlay exec"), LinkedPins(Caller.BeginPlay, UEdGraphSchema_K2::PN_Then, EventMessage, UEdGraphSchema_K2::PN_Execute));
		TestTrue(TEXT("... the target"), LinkedPins(Caller.Self, UEdGraphSchema_K2::PN_Self, EventMessage, UEdGraphSchema_K2::PN_Self));
		TestTrue(TEXT("... the continuation"), LinkedPins(EventMessage, UEdGraphSchema_K2::PN_Then, Caller.Consumer, UEdGraphSchema_K2::PN_Execute));
		TestTrue(TEXT("... and the renamed output reaches the Out setter through the new pin name"), LinkedPins(EventMessage, TEXT("ReturnValue"), Caller.Consumer, TEXT("Out")));
		TestTrue(TEXT("The Out setter keeps its identity"), Caller.Consumer->NodeGuid == ConsumerGuid);
		TestTrue(TEXT("The Self node keeps its identity"), Caller.Self->NodeGuid == SelfGuid);
		TestTrue(TEXT("The BeginPlay event keeps its identity"), Caller.BeginPlay->NodeGuid == BeginPlayGuid);

		UK2Node_DynamicCast* const RetargetedCast = Cast<UK2Node_DynamicCast>(FCortexGraphMigrationOps::FindNodeByGuidInGraph(Events, CastGuid));
		UK2Node_CallFunction* const Typed = Cast<UK2Node_CallFunction>(FCortexGraphMigrationOps::FindNodeByGuidInGraph(Events, TypedGuid));
		if (TestNotNull(TEXT("The cast keeps its GUID"), RetargetedCast) && TestNotNull(TEXT("The typed call keeps its GUID"), Typed))
		{
			TestTrue(TEXT("The cast now targets the native interface"), RetargetedCast->TargetType.Get() == Native);
			UEdGraphPin* const Result = RetargetedCast->GetCastResultPin();
			if (TestNotNull(TEXT("The recreated cast owns its result pin"), Result))
			{
				UEdGraphPin* const Self = Typed->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
				TestTrue(TEXT("... which feeds the typed call's target"), Self && Result->LinkedTo.Contains(Self) && Self->LinkedTo.Contains(Result));
				TestTrue(TEXT("... with the new class's pin name"), Result->PinName.ToString().Contains(TEXT("Cortex")));
			}
			TestTrue(TEXT("The cast keeps its exec links"), LinkedPins(Caller.Consumer, UEdGraphSchema_K2::PN_Then, RetargetedCast, UEdGraphSchema_K2::PN_Execute)
				&& RetargetedCast->GetValidCastPin() && RetargetedCast->GetValidCastPin()->LinkedTo.Contains(Typed->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)));
			TestTrue(TEXT("... and its source object link"), LinkedPins(Caller.Self, UEdGraphSchema_K2::PN_Self, RetargetedCast, UEdGraphSchema_K2::PN_ObjectToCast));
			TestTrue(TEXT("The typed call now names the native interface"),
				Typed->FunctionReference.GetMemberParentClass(Typed->GetBlueprintClassFromNode())->GetAuthoritativeClass() == Native);
			TestNotNull(TEXT("... and its output carries the new pin name"), Typed->FindPin(TEXT("ReturnValue"), EGPD_Output));
		}

		TestEqual(TEXT("The unrelated same-named call is untouched (pins, PinIds, links, reference)"), NodeDigest(Caller.Unrelated), UnrelatedBefore);
		FString FirstRemaining;
		TestEqual(TEXT("No site of the old endpoint remains"),
			FCortexGraphRetargetOps::CountRemainingSites(Caller.Blueprint, Interface->GeneratedClass, TEXT("GetValue"), FirstRemaining), 0);
		TestTrue(TEXT("The unrelated call still names its own class"),
			Caller.Unrelated->FunctionReference.GetMemberParentClass() == UCortexIssue122RetargetUnrelated::StaticClass());
	}

	// message -> call: a replacement node at the derived identity, the edges moved, the old node gone.
	{
		UK2Node_Message* Message = nullptr;
		UK2Node_Self* NativeSelf = nullptr;
		UK2Node_Event* NativeBeginPlay = nullptr;
		UBlueprint* CallerBlueprint = nullptr;
		if (!TestTrue(TEXT("Implementing caller"), MakeImplementingCaller(Scope, Interface, TEXT("BP_Issue122RetargetCall"), Message, NativeSelf, NativeBeginPlay, CallerBlueprint)))
		{
			return false;
		}
		const FGuid OldGuid = Message->NodeGuid;
		const FGuid SelfNodeGuid = NativeSelf->NodeGuid;
		UEdGraph* const Events = FBlueprintEditorUtils::FindEventGraph(CallerBlueprint);
		const int32 NodeCountBefore = Events->Nodes.Num();

		FCortexGraphPreparedPatch Preview1;
		FCortexCommandResult PreviewError;
		TestTrue(FString::Printf(TEXT("The mode call preview succeeds [%s]"), *PreviewError.ErrorMessage),
			CortexGraphRetargetTest::Preview(CallerBlueprint, Interface, ValuePinMap(), TEXT("call"), Preview1, PreviewError));
		const TSharedPtr<FJsonObject> Site = SiteByNode(Preview1, OldGuid);
		const FGuid Expected = FCortexGraphPatchOps::DeriveNodeGuid(Preview1.PatchId, FString(TEXT("retarget:")) + OldGuid.ToString());
		if (TestTrue(TEXT("The message site is published"), Site.IsValid()))
		{
			TestEqual(TEXT("... with the derived replacement identity"), Site->GetStringField(TEXT("replacement_guid")), Expected.ToString());
			TestFalse(TEXT("... and not blocked: the target pin connects directly"), Site->HasField(TEXT("blocked")));
		}

		FCompileCounter Counter;
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		const bool bApplied = FCortexGraphPatchOps::Execute(CallerBlueprint,
			ApplyRequest(CallerBlueprint, Interface, ValuePinMap(), TEXT("call"), Preview1), Outcome, Error);
		TestTrue(FString::Printf(TEXT("The message -> call apply succeeds [%s: %s] [%s]"), *Error.ErrorCode, *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))), bApplied);
		if (bApplied)
		{
			TestEqual(TEXT("Readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
			TestEqual(TEXT("One target compile"), Counter.TargetCompiles, 1);
			TestTrue(TEXT("The asset compiles"), CallerBlueprint->Status == BS_UpToDate);
			TestNull(TEXT("The message node is gone"), FCortexGraphMigrationOps::FindNodeByGuidInGraph(Events, OldGuid));
			UEdGraphNode* const Replacement = FCortexGraphMigrationOps::FindNodeByGuidInGraph(Events, Expected);
			if (TestNotNull(TEXT("The replacement exists at the derived GUID"), Replacement))
			{
				TestTrue(TEXT("... as a direct typed call"), Replacement->GetClass() == UK2Node_CallFunction::StaticClass());
				const UK2Node_CallFunction* const Call = Cast<UK2Node_CallFunction>(Replacement);
				TestTrue(TEXT("... of the native interface function"), Call->FunctionReference.GetMemberParentClass(Call->GetBlueprintClassFromNode())->GetAuthoritativeClass() == Native);
				UEdGraphNode* const SelfNode = FCortexGraphMigrationOps::FindNodeByGuidInGraph(Events, SelfNodeGuid);
				TestTrue(TEXT("... with the exec edge moved"), LinkedPins(NativeBeginPlay, UEdGraphSchema_K2::PN_Then, Replacement, UEdGraphSchema_K2::PN_Execute));
				TestTrue(TEXT("... and the target edge moved"), LinkedPins(SelfNode, UEdGraphSchema_K2::PN_Self, Replacement, UEdGraphSchema_K2::PN_Self));
				TestEqual(TEXT("... and the node count is unchanged (one out, one in)"), Events->Nodes.Num(), NodeCountBefore);
				TestTrue(TEXT("... the replacement goes at the end of the node list"), Events->Nodes.Last() == Replacement);
			}
			const FGuid* const Reported = Outcome.Locators.NodeGuidByClientId.Find(FString(TEXT("retarget:")) + OldGuid.ToString());
			TestTrue(TEXT("The outcome reports the replacement identity"), Reported && *Reported == Expected);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationRetargetStaleEchoTest,
	"Cortex.Graph.Authoring.Migration.Retarget.StaleEcho",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetargetStaleEchoTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphRetargetTest;

	FScope Scope;
	UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122RetargetStale"));
	FCaller Caller;
	if (!TestNotNull(TEXT("BPI"), Interface) || !TestTrue(TEXT("Caller"), MakeCaller(Scope, Interface, TEXT("BP_Issue122RetargetStale"), Caller)))
	{
		return false;
	}
	FCortexGraphPreparedPatch Preview1;
	FCortexCommandResult PreviewError;
	if (!TestTrue(TEXT("Preview"), Preview(Caller.Blueprint, Interface, ValuePinMap(), nullptr, Preview1, PreviewError)))
	{
		return false;
	}
	const FString Before = Snapshot(Caller.Blueprint);
	const bool bDirtyBefore = Caller.Blueprint->GetOutermost()->IsDirty();
	const TArray<TSharedPtr<FJsonValue>> Sites = Preview1.RetargetPlan->GetArrayField(TEXT("sites"));

	const auto ApplyWith = [&](const TArray<TSharedPtr<FJsonValue>>& Echo, const FString& Hash)
	{
		TSharedPtr<FJsonObject> Apply = Request(Caller.Blueprint, Migration(Interface, ValuePinMap(), nullptr, &Echo), /*bDryRun=*/false);
		Apply->SetStringField(TEXT("expected_validation_hash"), Hash);
		return Apply;
	};
	const auto ExpectRefused = [&](const TCHAR* Label, const TSharedPtr<FJsonObject>& Apply, const FString& ExpectedCode, const TCHAR* ExpectedText)
	{
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		TestFalse(FString::Printf(TEXT("%s is refused"), Label), FCortexGraphPatchOps::Execute(Caller.Blueprint, Apply, Outcome, Error));
		TestEqual(FString::Printf(TEXT("%s: error code [%s]"), Label, *Error.ErrorMessage), Error.ErrorCode, ExpectedCode);
		TestTrue(FString::Printf(TEXT("%s: the refusal names '%s' [%s]"), Label, ExpectedText, *Error.ErrorMessage), Error.ErrorMessage.Contains(ExpectedText));
		TestEqual(FString::Printf(TEXT("%s: nothing was applied"), Label), Outcome.ApplyStatus, FString(TEXT("not_requested")));
		TestEqual(FString::Printf(TEXT("%s: no compile ran"), Label), Outcome.TargetCompileCount + Outcome.RecoveryCompileCount, 0);
		TestEqual(FString::Printf(TEXT("%s: the asset is exactly as it was"), Label), Snapshot(Caller.Blueprint), Before);
		TestEqual(FString::Printf(TEXT("%s: no package was dirtied"), Label), Caller.Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);
	};

	// An echo missing one site.
	{
		TArray<TSharedPtr<FJsonValue>> Missing = Sites;
		Missing.RemoveAt(0);
		ExpectRefused(TEXT("An echo missing a site"), ApplyWith(Missing, Preview1.ValidationHash), CortexErrorCodes::InvalidOperation, TEXT("missing"));
	}
	// An echo with an extra site (a node that is not a site of the old endpoint).
	{
		TArray<TSharedPtr<FJsonValue>> Extra = Sites;
		const FString UnrelatedGuid = Caller.Unrelated->NodeGuid.ToString();
		TSharedPtr<FJsonObject> Fabricated = CloneObject(Sites[0]->AsObject());
		Fabricated->SetStringField(TEXT("node_guid"), UnrelatedGuid);
		Extra.Add(MakeShared<FJsonValueObject>(Fabricated));
		ExpectRefused(TEXT("An echo with an extra site"), ApplyWith(Extra, Preview1.ValidationHash), CortexErrorCodes::InvalidOperation, *UnrelatedGuid);
	}
	// An echo whose site was re-linked: one link of the event message's Value pin points somewhere else.
	{
		TArray<TSharedPtr<FJsonValue>> Relinked;
		for (const TSharedPtr<FJsonValue>& Value : Sites)
		{
			TSharedPtr<FJsonObject> Site = CloneObject(Value->AsObject());
			if (Site->GetStringField(TEXT("node_guid")) == Caller.EventMessage->NodeGuid.ToString())
			{
				TArray<TSharedPtr<FJsonValue>> Pins;
				for (const TSharedPtr<FJsonValue>& PinValue : Site->GetArrayField(TEXT("pins")))
				{
					TSharedPtr<FJsonObject> Pin = CloneObject(PinValue->AsObject());
					if (Pin->GetStringField(TEXT("name")) == TEXT("Value"))
					{
						TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
						Link->SetStringField(TEXT("node_guid"), Caller.Cast->NodeGuid.ToString());
						Link->SetStringField(TEXT("pin"), TEXT("Out"));
						TArray<TSharedPtr<FJsonValue>> Links;
						Links.Add(MakeShared<FJsonValueObject>(Link));
						Pin->SetArrayField(TEXT("links"), Links);
					}
					Pins.Add(MakeShared<FJsonValueObject>(Pin));
				}
				Site->SetArrayField(TEXT("pins"), Pins);
			}
			Relinked.Add(MakeShared<FJsonValueObject>(Site));
		}
		ExpectRefused(TEXT("An echo with a re-linked pin"), ApplyWith(Relinked, Preview1.ValidationHash), CortexErrorCodes::InvalidOperation, TEXT("re-linked"));
	}
	// A stale validation hash with the exact echo.
	ExpectRefused(TEXT("A stale validation hash"), ApplyWith(Sites, TEXT("deadbeef")), CortexErrorCodes::StalePrecondition, TEXT("expected_validation_hash"));
	// The asset re-linked after the preview: the preview's echo and hash no longer describe it.
	{
		UEdGraphPin* const Value = Caller.EventMessage->FindPin(TEXT("Value"), EGPD_Output);
		UEdGraphPin* const Out = Caller.Consumer->FindPin(TEXT("Out"), EGPD_Input);
		Value->BreakLinkTo(Out);
		const FString AfterEdit = Snapshot(Caller.Blueprint);
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		TestFalse(TEXT("An echo reviewed before a link was removed is refused"),
			FCortexGraphPatchOps::Execute(Caller.Blueprint, ApplyWith(Sites, Preview1.ValidationHash), Outcome, Error));
		TestTrue(FString::Printf(TEXT("... as a changed site or a stale precondition [%s: %s]"), *Error.ErrorCode, *Error.ErrorMessage),
			Error.ErrorCode == CortexErrorCodes::InvalidOperation || Error.ErrorCode == CortexErrorCodes::StalePrecondition);
		TestEqual(TEXT("... without applying anything"), Outcome.ApplyStatus, FString(TEXT("not_requested")));
		TestEqual(TEXT("... and without touching the asset"), Snapshot(Caller.Blueprint), AfterEdit);
		TestTrue(TEXT("Re-making the link restores the reviewed state"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Value, Out));
		TestEqual(TEXT("... exactly"), Snapshot(Caller.Blueprint), Before);
	}

	// message -> call onto a target pin that cannot connect is blocked: target_incompatible, and the apply refuses.
	{
		UK2Node_Message* Message = nullptr;
		UBlueprint* SelfCaller = nullptr;
		if (TestTrue(TEXT("Self caller"), MakeSelfCaller(Scope, Interface, TEXT("BP_Issue122RetargetSelf"), Message, SelfCaller)))
		{
			const FString SelfBefore = Snapshot(SelfCaller);
			const bool bSelfDirtyBefore = SelfCaller->GetOutermost()->IsDirty();
			FCortexGraphPreparedPatch CallPreview;
			FCortexCommandResult CallError;
			TestTrue(FString::Printf(TEXT("The preview publishes the blocked site [%s]"), *CallError.ErrorMessage),
				CortexGraphRetargetTest::Preview(SelfCaller, Interface, ValuePinMap(), TEXT("call"), CallPreview, CallError));
			const TSharedPtr<FJsonObject> Site = SiteByNode(CallPreview, Message->NodeGuid);
			if (TestTrue(TEXT("The message site is published"), Site.IsValid()))
			{
				TestTrue(FString::Printf(TEXT("... blocked: target_incompatible [%s]"), *Site->GetStringField(TEXT("blocked"))),
					Site->GetStringField(TEXT("blocked")).Contains(TEXT("target_incompatible")));
			}
			FCortexGraphPatchOutcome Outcome;
			FCortexCommandResult Error;
			TestFalse(TEXT("The apply of the blocked plan is refused"), FCortexGraphPatchOps::Execute(SelfCaller,
				ApplyRequest(SelfCaller, Interface, ValuePinMap(), TEXT("call"), CallPreview), Outcome, Error));
			TestEqual(TEXT("... as INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
			TestTrue(TEXT("... naming target_incompatible"), Error.ErrorMessage.Contains(TEXT("target_incompatible")));
			TestEqual(TEXT("... without applying"), Outcome.ApplyStatus, FString(TEXT("not_requested")));
			TestEqual(TEXT("... and without touching the asset"), Snapshot(SelfCaller), SelfBefore);
			TestEqual(TEXT("... or its package"), SelfCaller->GetOutermost()->IsDirty(), bSelfDirtyBefore);
		}
	}

	// A link the schema could only re-make by BREAK_OTHERS (an exec output that fans out to a node outside the sites) blocks the
	// site: the retarget never breaks a link nobody reviewed.
	{
		UK2Node_Message* Message = nullptr;
		UK2Node_Self* FanSelf = nullptr;
		UK2Node_Event* FanBeginPlay = nullptr;
		UBlueprint* Fan = nullptr;
		if (TestTrue(TEXT("Fan-out caller"), MakeImplementingCaller(Scope, Interface, TEXT("BP_Issue122RetargetFan"), Message, FanSelf, FanBeginPlay, Fan)))
		{
			UEdGraph* const Events = FBlueprintEditorUtils::FindEventGraph(Fan);
			UK2Node_ExecutionSequence* const Other = AddNode<UK2Node_ExecutionSequence>(Events);
			UEdGraphPin* const Then = FanBeginPlay->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			Then->MakeLinkTo(Other->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
			const FString FanBefore = Snapshot(Fan);
			FCortexGraphPreparedPatch FanPreview;
			FCortexCommandResult FanError;
			TestTrue(FString::Printf(TEXT("The preview publishes the blocked site [%s]"), *FanError.ErrorMessage),
				CortexGraphRetargetTest::Preview(Fan, Interface, ValuePinMap(), TEXT("call"), FanPreview, FanError));
			const TSharedPtr<FJsonObject> Site = SiteByNode(FanPreview, Message->NodeGuid);
			if (TestTrue(TEXT("The message site is published"), Site.IsValid()))
			{
				TestTrue(FString::Printf(TEXT("... blocked: the exec link cannot be re-made without BREAK_OTHERS [%s]"), *Site->GetStringField(TEXT("blocked"))),
					Site->GetStringField(TEXT("blocked")).Contains(TEXT("link_incompatible")));
			}
			FCortexGraphPatchOutcome Outcome;
			FCortexCommandResult Error;
			TestFalse(TEXT("The apply is refused"), FCortexGraphPatchOps::Execute(Fan,
				ApplyRequest(Fan, Interface, ValuePinMap(), TEXT("call"), FanPreview), Outcome, Error));
			TestEqual(TEXT("... as INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
			TestEqual(TEXT("... without applying"), Outcome.ApplyStatus, FString(TEXT("not_requested")));
			TestEqual(TEXT("... and the unreviewed link was never broken"), Snapshot(Fan), FanBefore);
			TestTrue(TEXT("... the fan-out target is still linked"), LinkedPins(FanBeginPlay, UEdGraphSchema_K2::PN_Then, Other, UEdGraphSchema_K2::PN_Execute));
		}
	}
	TestEqual(TEXT("Every refusal left the first fixture exactly as it was"), Snapshot(Caller.Blueprint), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationRetargetRollbackTest,
	"Cortex.Graph.Authoring.Migration.Retarget.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetargetRollbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphRetargetTest;

	FScope Scope;
	UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122RetargetRollback"));
	FCaller Caller;
	UK2Node_Message* CallMessage = nullptr;
	UK2Node_Self* CallSelf = nullptr;
	UK2Node_Event* CallBeginPlay = nullptr;
	UBlueprint* CallBlueprint = nullptr;
	if (!TestNotNull(TEXT("BPI"), Interface) || !TestTrue(TEXT("Caller"), MakeCaller(Scope, Interface, TEXT("BP_Issue122RetargetRollback"), Caller))
		|| !TestTrue(TEXT("Implementing caller"), MakeImplementingCaller(Scope, Interface, TEXT("BP_Issue122RetargetRollbackCall"), CallMessage, CallSelf, CallBeginPlay, CallBlueprint)))
	{
		return false;
	}

	struct FFault { const TCHAR* Label; const TCHAR* Apply; const TCHAR* Readback; bool bMutateBeforeReadback; };
	const FFault Faults[] = {
		{ TEXT("after the first site"), TEXT("migration_retarget_after_first_site"), nullptr, false },
		{ TEXT("after every site"), TEXT("migration_retarget_after_sites"), nullptr, false },
		{ TEXT("readback node set divergence"), nullptr, TEXT("retarget_nodes"), false },
		{ TEXT("readback link divergence"), nullptr, TEXT("retarget_links"), false },
		{ TEXT("native state divergence between apply and readback"), nullptr, nullptr, true },
	};
	struct FCase { const TCHAR* Label; UBlueprint* Blueprint; const TCHAR* Mode; };
	const FCase Cases[] = {
		{ TEXT("message mode"), Caller.Blueprint, nullptr },
		{ TEXT("call mode"), CallBlueprint, TEXT("call") },
	};

	for (const FCase& Case : Cases)
	{
		const FString Before = Snapshot(Case.Blueprint);
		const bool bDirtyBefore = Case.Blueprint->GetOutermost()->IsDirty();
		for (const FFault& Fault : Faults)
		{
			FCortexGraphPreparedPatch Prepared;
			FCortexCommandResult PreviewError;
			if (!TestTrue(FString::Printf(TEXT("[%s %s] preview"), Case.Label, Fault.Label),
				Preview(Case.Blueprint, Interface, ValuePinMap(), Case.Mode, Prepared, PreviewError)))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Apply = ApplyRequest(Case.Blueprint, Interface, ValuePinMap(), Case.Mode, Prepared);

			FCompileCounter Counter;
			if (Fault.Apply != nullptr) { FCortexGraphPatchOps::SetApplyFaultPointForTesting(FName(Fault.Apply)); }
			if (Fault.Readback != nullptr) { FCortexGraphPatchOps::SetReadbackFaultForTesting(FName(Fault.Readback)); }
			if (Fault.bMutateBeforeReadback)
			{
				FCortexGraphPatchOps::SetPreReadbackMutatorForTesting([](UBlueprint* Target)
				{
					// A real divergence of the native state: a retargeted node loses one of its links before the readback.
					for (UEdGraph* Graph : Target->UbergraphPages)
					{
						for (UEdGraphNode* Node : Graph->Nodes)
						{
							UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
							if (Call != nullptr && Call->FunctionReference.GetMemberParentClass() == UCortexIssue122GraphInterface::StaticClass())
							{
								Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)->BreakAllPinLinks();
								return;
							}
						}
					}
				});
			}
			FCortexGraphPatchOutcome Outcome;
			FCortexCommandResult Error;
			const bool bApplied = FCortexGraphPatchOps::Execute(Case.Blueprint, Apply, Outcome, Error);
			FCortexGraphPatchOps::ClearApplyFaultPointForTesting();
			FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);
			FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();

			TestFalse(FString::Printf(TEXT("[%s %s] the apply fails"), Case.Label, Fault.Label), bApplied);
			TestEqual(FString::Printf(TEXT("[%s %s] rollback restored [%s]"), Case.Label, Fault.Label, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
				Outcome.RollbackStatus, FString(TEXT("restored")));
			TestEqual(FString::Printf(TEXT("[%s %s] exactly one recovery compile"), Case.Label, Fault.Label), Counter.RecoveryCompiles, 1);
			TestEqual(FString::Printf(TEXT("[%s %s] the outcome reports it"), Case.Label, Fault.Label), Outcome.RecoveryCompileCount, 1);
			TestFalse(FString::Printf(TEXT("[%s %s] the asset is not blocked"), Case.Label, Fault.Label), Outcome.bBlocked);
			TestEqual(FString::Printf(TEXT("[%s %s] every node, pin, PinId, link and the node order are exactly restored"), Case.Label, Fault.Label),
				Snapshot(Case.Blueprint), Before);
			TestEqual(FString::Printf(TEXT("[%s %s] the dirty flag is restored"), Case.Label, Fault.Label), Case.Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);
			TestTrue(FString::Printf(TEXT("[%s %s] the restored asset compiles"), Case.Label, Fault.Label), Case.Blueprint->Status == BS_UpToDate);
		}
	}

	// The restored assets are coherent: the same request now applies cleanly.
	for (const FCase& Case : Cases)
	{
		FCompileCounter Counter;
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		const bool bApplied = RunApply(Case.Blueprint, Interface, Case.Mode, Outcome, Error);
		TestTrue(FString::Printf(TEXT("[%s] a final apply succeeds on the restored asset [%s: %s]"), Case.Label, *Error.ErrorCode, *Error.ErrorMessage), bApplied);
		TestEqual(FString::Printf(TEXT("[%s] ... with one target compile"), Case.Label), Counter.TargetCompiles, 1);
		TestEqual(FString::Printf(TEXT("[%s] ... and no recovery"), Case.Label), Counter.RecoveryCompiles, 0);
	}
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
