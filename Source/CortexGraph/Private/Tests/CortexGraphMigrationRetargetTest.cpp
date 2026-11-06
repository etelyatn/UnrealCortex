// retarget_calls preview (#122 Task 10): the per-site pin and link inventory with a stable validation hash and awaiting_approval, the
// refusals that leave the asset untouched, and the matching by resolved owner (an unrelated same-named call is never a site).

#include "Misc/AutomationTest.h"

#include "CortexGraphRetargetTestSupport.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationRetargetPreviewTest,
	"Cortex.Graph.Authoring.Migration.Retarget.Preview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetargetPreviewTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphRetargetTest;

	FScope Scope;
	UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122RetargetPreview"));
	FCaller Caller;
	if (!TestNotNull(TEXT("BPI"), Interface) || !TestTrue(TEXT("Caller"), MakeCaller(Scope, Interface, TEXT("BP_Issue122RetargetPreview"), Caller)))
	{
		return false;
	}
	const FString Before = Snapshot(Caller.Blueprint);
	const bool bDirtyBefore = Caller.Blueprint->GetOutermost()->IsDirty();

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	const bool bPreviewed = Preview(Caller.Blueprint, Interface, ValuePinMap(), nullptr, Prepared, Error);
	TestTrue(FString::Printf(TEXT("The preview succeeds [%s: %s]"), *Error.ErrorCode, *Error.ErrorMessage), bPreviewed);
	if (!bPreviewed)
	{
		return false;
	}
	TestTrue(TEXT("The preview is a dry run of a retarget plan"), Prepared.bDryRun && Prepared.bIsRetarget());
	TestFalse(TEXT("A validation hash is published"), Prepared.ValidationHash.IsEmpty());
	TestFalse(TEXT("A preview that only publishes the inventory changes nothing"), Prepared.bChanged);

	FCortexGraphMigrationRetargetPlan Plan;
	FCortexCommandResult PlanError;
	if (TestTrue(TEXT("The durable plan round-trips"), FCortexGraphMigrationRetargetPlan::FromJson(Prepared.RetargetPlan, Plan, PlanError)))
	{
		TestTrue(TEXT("The plan awaits the caller's approval"), Plan.bAwaitingApproval);
		TestEqual(TEXT("The new owner is the native interface"), Plan.ToOwner, NativePath());
		TestEqual(TEXT("An interface target defaults to message mode"), Plan.Mode, FString(TEXT("message")));
		TestFalse(TEXT("Nothing is blocked"), Plan.HasBlockedSite());
		TestEqual(TEXT("Exactly the five sites are published: three messages, one typed call and one cast"), Plan.Sites.Num(), 5);
		int32 Messages = 0;
		int32 Typed = 0;
		int32 Casts = 0;
		TSet<FString> Graphs;
		for (const FCortexGraphRetargetSite& Site : Plan.Sites)
		{
			Messages += Site.Kind == TEXT("message") ? 1 : 0;
			Typed += Site.Kind == TEXT("typed_call") ? 1 : 0;
			Casts += Site.Kind == TEXT("interface_cast") ? 1 : 0;
			Graphs.Add(Site.GraphGuid);
			TestTrue(TEXT("The site is never the unrelated same-named call"), Site.NodeGuid != Caller.Unrelated->NodeGuid.ToString());
			TestFalse(TEXT("Every site publishes its pins"), Site.Pins.IsEmpty());
		}
		TestEqual(TEXT("Three message sites (event graph, function graph, nested composite)"), Messages, 3);
		TestEqual(TEXT("One typed call"), Typed, 1);
		TestEqual(TEXT("One cast"), Casts, 1);
		TestEqual(TEXT("The sites live in three graphs (event, function and nested)"), Graphs.Num(), 3);
		TestEqual(TEXT("A preservation contract is captured per touched graph"), Plan.Preservations.Num(), 3);
	}

	// Per-pin {name, direction, signature, links}.
	const TSharedPtr<FJsonObject> EventSite = SiteByNode(Prepared, Caller.EventMessage->NodeGuid);
	if (TestTrue(TEXT("The event-graph message is published"), EventSite.IsValid()))
	{
		TestEqual(TEXT("Kind"), EventSite->GetStringField(TEXT("kind")), FString(TEXT("message")));
		TestEqual(TEXT("Node class"), EventSite->GetStringField(TEXT("node_class_path")), UK2Node_Message::StaticClass()->GetPathName());
		const TSharedPtr<FJsonObject> Value = PinOfSite(EventSite, TEXT("Value"), TEXT("output"));
		if (TestTrue(TEXT("The Value output pin is published"), Value.IsValid()))
		{
			TestFalse(TEXT("... with a canonical signature"), Value->GetStringField(TEXT("signature")).IsEmpty());
			TestEqual(TEXT("... that maps onto ReturnValue through pin_map"), Value->GetStringField(TEXT("maps_to")), FString(TEXT("ReturnValue")));
			const TArray<TSharedPtr<FJsonValue>>& Links = Value->GetArrayField(TEXT("links"));
			TestEqual(TEXT("... with its one consumer link"), Links.Num(), 1);
			if (Links.Num() == 1)
			{
				TestEqual(TEXT("... to the Out setter"), Links[0]->AsObject()->GetStringField(TEXT("node_guid")), Caller.Consumer->NodeGuid.ToString());
				TestEqual(TEXT("... on its Out pin"), Links[0]->AsObject()->GetStringField(TEXT("pin")), FString(TEXT("Out")));
			}
		}
		TestTrue(TEXT("The exec input is published with its link"),
			PinOfSite(EventSite, TEXT("execute"), TEXT("input")).IsValid()
			&& PinOfSite(EventSite, TEXT("execute"), TEXT("input"))->GetArrayField(TEXT("links")).Num() == 1);
	}
	const TSharedPtr<FJsonObject> CastSite = SiteByNode(Prepared, Caller.Cast->NodeGuid);
	if (TestTrue(TEXT("The cast is published"), CastSite.IsValid()))
	{
		TestEqual(TEXT("Cast kind"), CastSite->GetStringField(TEXT("kind")), FString(TEXT("interface_cast")));
		const UEdGraphPin* const Result = Caller.Cast->GetCastResultPin();
		const TSharedPtr<FJsonObject> ResultPin = Result ? PinOfSite(CastSite, *Result->PinName.ToString(), TEXT("output")) : nullptr;
		if (TestTrue(TEXT("The cast result pin is published"), ResultPin.IsValid()))
		{
			TestFalse(TEXT("... mapped onto the replacement cast result pin"), ResultPin->GetStringField(TEXT("maps_to")).IsEmpty());
			TestTrue(TEXT("... which is named for the new class"), ResultPin->GetStringField(TEXT("maps_to")) != Result->PinName.ToString());
		}
	}
	TestTrue(TEXT("The typed call is published"), SiteByNode(Prepared, Caller.Typed->NodeGuid).IsValid());
	TestTrue(TEXT("The function-graph message is published"), SiteByNode(Prepared, Caller.HelperMessage->NodeGuid).IsValid());
	TestTrue(TEXT("The nested composite message is published"), SiteByNode(Prepared, Caller.NestedMessage->NodeGuid).IsValid());
	TestFalse(TEXT("The unrelated same-named call is not published"), SiteByNode(Prepared, Caller.Unrelated->NodeGuid).IsValid());

	// The inventory the handler publishes.
	const TSharedPtr<FJsonObject> Inventory = FCortexGraphRetargetOps::MakeInventory(Prepared.RetargetPlan);
	if (TestTrue(TEXT("The inventory is published"), Inventory.IsValid()))
	{
		TestTrue(TEXT("... awaiting approval"), Inventory->GetBoolField(TEXT("awaiting_approval")));
		TestEqual(TEXT("... with its site count"), static_cast<int32>(Inventory->GetNumberField(TEXT("site_count"))), 5);
		TestEqual(TEXT("... and no blocked site"), static_cast<int32>(Inventory->GetNumberField(TEXT("blocked_count"))), 0);
		TestTrue(TEXT("... complete"), Inventory->GetBoolField(TEXT("complete")));
	}

	// The validation hash is stable, changes nothing, and does not depend on whether the reviewed sites were echoed.
	{
		FCortexGraphPreparedPatch Again;
		FCortexCommandResult AgainError;
		TestTrue(TEXT("A second preview succeeds"), Preview(Caller.Blueprint, Interface, ValuePinMap(), nullptr, Again, AgainError));
		TestEqual(TEXT("The validation hash is stable for an identical request"), Again.ValidationHash, Prepared.ValidationHash);

		const TArray<TSharedPtr<FJsonValue>> Sites = Prepared.RetargetPlan->GetArrayField(TEXT("sites"));
		FCortexGraphPreparedPatch Echoed;
		FCortexCommandResult EchoError;
		TestTrue(FString::Printf(TEXT("A preview that echoes the sites succeeds [%s]"), *EchoError.ErrorMessage),
			FCortexGraphPatchOps::Preflight(Caller.Blueprint, Request(Caller.Blueprint, Migration(Interface, ValuePinMap(), nullptr, &Sites)), Echoed, EchoError));
		TestEqual(TEXT("The hash of the echoed request is the hash of the preview the caller approves"), Echoed.ValidationHash, Prepared.ValidationHash);
		TestFalse(TEXT("... and it is no longer awaiting approval"), Echoed.RetargetPlan->GetBoolField(TEXT("awaiting_approval")));
	}
	TestEqual(TEXT("The previews changed no node, pin, link or graph"), Snapshot(Caller.Blueprint), Before);
	TestEqual(TEXT("The previews did not touch the dirty flag"), Caller.Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);

	// A refusal changes nothing.
	const auto ExpectRefusal = [&](const TCHAR* Label, const TSharedPtr<FJsonObject>& RequestJson, const FString& ExpectedCode, const TCHAR* ExpectedText)
	{
		FCortexGraphPreparedPatch Refused;
		FCortexCommandResult RefusalError;
		TestFalse(FString::Printf(TEXT("%s is refused"), Label), FCortexGraphPatchOps::Preflight(Caller.Blueprint, RequestJson, Refused, RefusalError));
		TestEqual(FString::Printf(TEXT("%s: error code [%s]"), Label, *RefusalError.ErrorMessage), RefusalError.ErrorCode, ExpectedCode);
		TestTrue(FString::Printf(TEXT("%s: the refusal names '%s' [%s]"), Label, ExpectedText, *RefusalError.ErrorMessage), RefusalError.ErrorMessage.Contains(ExpectedText));
		TestEqual(FString::Printf(TEXT("%s: nothing changed"), Label), Snapshot(Caller.Blueprint), Before);
	};
	{
		// `target` must be absent: the op addresses every graph through the sites it discovers.
		TSharedPtr<FJsonObject> WithTarget = Request(Caller.Blueprint, Migration(Interface, ValuePinMap()));
		WithTarget->SetObjectField(TEXT("target"), MakeShared<FJsonObject>());
		ExpectRefusal(TEXT("A request carrying target"), WithTarget, CortexErrorCodes::InvalidField, TEXT("target"));
	}
	{
		// An apply without the reviewed sites is refused before anything is retargeted.
		TSharedPtr<FJsonObject> Apply = Request(Caller.Blueprint, Migration(Interface, ValuePinMap()), /*bDryRun=*/false);
		Apply->SetStringField(TEXT("expected_validation_hash"), Prepared.ValidationHash);
		ExpectRefusal(TEXT("An apply without sites"), Apply, CortexErrorCodes::InvalidField, TEXT("migration.sites"));
	}
	{
		TSharedPtr<FJsonObject> Bad = Migration(Interface, ValuePinMap());
		TSharedPtr<FJsonObject> To = Endpoint(NativePath());
		To->SetStringField(TEXT("function"), TEXT("NoSuchFunction"));
		Bad->SetObjectField(TEXT("to"), To);
		ExpectRefusal(TEXT("An unresolvable target function"), Request(Caller.Blueprint, Bad), CortexErrorCodes::InvalidField, TEXT("NoSuchFunction"));
	}
	{
		TSharedPtr<FJsonObject> Bad = Migration(Interface, ValuePinMap());
		Bad->SetObjectField(TEXT("to"), Endpoint(TEXT("/Script/NoSuchModule.NoSuchClass")));
		ExpectRefusal(TEXT("An unresolvable target owner"), Request(Caller.Blueprint, Bad), CortexErrorCodes::InvalidField, TEXT("migration.to.owner"));
	}
	{
		TSharedPtr<FJsonObject> Bad = Migration(Interface, ValuePinMap());
		Bad->SetStringField(TEXT("sites_typo"), TEXT("x"));
		ExpectRefusal(TEXT("An unknown migration field"), Request(Caller.Blueprint, Bad), CortexErrorCodes::InvalidField, TEXT("sites_typo"));
	}
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("from_pin"), TEXT("NoSuchPin"));
		Entry->SetStringField(TEXT("to_pin"), TEXT("ReturnValue"));
		TArray<TSharedPtr<FJsonValue>> Map;
		Map.Add(MakeShared<FJsonValueObject>(Entry));
		ExpectRefusal(TEXT("A pin_map entry no site carries"), Request(Caller.Blueprint, Migration(Interface, Map)), CortexErrorCodes::InvalidField, TEXT("NoSuchPin"));
	}
	{
		// Without the pin_map the renamed Value output has links but no counterpart: the site is published as blocked.
		FCortexGraphPreparedPatch Unmapped;
		FCortexCommandResult UnmappedError;
		TestTrue(TEXT("A preview without the pin_map is still published"), Preview(Caller.Blueprint, Interface, TArray<TSharedPtr<FJsonValue>>(), nullptr, Unmapped, UnmappedError));
		const TSharedPtr<FJsonObject> Site = SiteByNode(Unmapped, Caller.EventMessage->NodeGuid);
		TestTrue(TEXT("... with the Value output reported as unmapped"), Site.IsValid() && Site->GetStringField(TEXT("blocked")).Contains(TEXT("pin_unmapped")));
		TestEqual(TEXT("... and the blocked count"), static_cast<int32>(FCortexGraphRetargetOps::MakeInventory(Unmapped.RetargetPlan)->GetNumberField(TEXT("blocked_count"))), 1);

		// A blocked plan cannot be applied even with the exact echo.
		const TArray<TSharedPtr<FJsonValue>> Sites = Unmapped.RetargetPlan->GetArrayField(TEXT("sites"));
		TSharedPtr<FJsonObject> Apply = Request(Caller.Blueprint, Migration(Interface, TArray<TSharedPtr<FJsonValue>>(), nullptr, &Sites), /*bDryRun=*/false);
		Apply->SetStringField(TEXT("expected_validation_hash"), Unmapped.ValidationHash);
		ExpectRefusal(TEXT("An apply of a blocked plan"), Apply, CortexErrorCodes::InvalidOperation, TEXT("pin_unmapped"));
	}
	{
		// A Blueprint beyond max_scanned_nodes is refused with its observed count (LIMIT_EXCEEDED).
		FCaller Big;
		if (TestTrue(TEXT("Big caller"), MakeCaller(Scope, Interface, TEXT("BP_Issue122RetargetBig"), Big)))
		{
			for (int32 Index = 0; Index < FCortexGraphPatchOps::MaxScannedNodes + 16; ++Index)
			{
				UK2Node_ExecutionSequence* Filler = NewObject<UK2Node_ExecutionSequence>(Big.EventGraph);
				Filler->CreateNewGuid();
				Filler->AllocateDefaultPins();
				Big.EventGraph->AddNode(Filler, true, false);
			}
			FCortexGraphPreparedPatch Refused;
			FCortexCommandResult RefusalError;
			TestFalse(TEXT("A Blueprint beyond max_scanned_nodes is refused"),
				FCortexGraphPatchOps::Preflight(Big.Blueprint, Request(Big.Blueprint, Migration(Interface, ValuePinMap())), Refused, RefusalError));
			TestEqual(TEXT("... as LIMIT_EXCEEDED"), RefusalError.ErrorCode, FString(CortexErrorCodes::LimitExceeded));
			TestTrue(TEXT("... naming max_scanned_nodes"), RefusalError.ErrorMessage.Contains(TEXT("max_scanned_nodes")));
		}
	}
	{
		// A Blueprint with no site of the old endpoint has nothing to retarget.
		UBlueprint* const Empty = MakeActorBlueprint(Scope, TEXT("BP_Issue122RetargetEmpty"));
		if (TestNotNull(TEXT("Empty Blueprint"), Empty))
		{
			FCortexGraphPreparedPatch Refused;
			FCortexCommandResult RefusalError;
			TestFalse(TEXT("A Blueprint without a site is refused"),
				FCortexGraphPatchOps::Preflight(Empty, Request(Empty, Migration(Interface, ValuePinMap())), Refused, RefusalError));
			TestEqual(TEXT("... as INVALID_OPERATION"), RefusalError.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		}
	}
	TestEqual(TEXT("Every refusal left the first fixture exactly as it was"), Snapshot(Caller.Blueprint), Before);
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
