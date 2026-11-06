// replace_interface fix round 1: implementations that live only in the terminators, mapped result literals, the complete
// replacement-boundary edge set, replay identity reporting and the published preservation capture.

#include "Misc/AutomationTest.h"

#include "CortexGraphReplaceInterfaceTestSupport.h"
#include "Operations/CortexGraphInterfaceMigration.h"
#include "Operations/CortexGraphMigrationOps.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphReplaceInterfaceFixTest
{
	using namespace CortexGraphReplaceInterfaceTest;

	UEdGraphNode* NodeIn(UEdGraph* Graph, const FGuid& Guid)
	{
		return FCortexGraphMigrationOps::FindNodeByGuidInGraph(Graph, Guid);
	}

	TArray<TSharedPtr<FJsonValue>> OnlyNotify()
	{
		TArray<TSharedPtr<FJsonValue>> Map;
		Map.Add(MapEntry(TEXT("Notify"), nullptr, EventPinMap()));
		return Map;
	}

	void ExpectOmissionRefused(FAutomationTestBase& Test, UBlueprint* Old, FImplementer& Impl, const TCHAR* Name, const TCHAR* Label)
	{
		const FString Before = StructureDigest(Impl.Blueprint);
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		const bool bPreviewed = FCortexGraphPatchOps::Preflight(Impl.Blueprint,
			Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122501"), MigrationJson(Old->GetPathName(), NativePath(), OnlyNotify())), Prepared, Error);
		Test.TestFalse(FString::Printf(TEXT("%s: omitting the implemented function is refused"), Label), bPreviewed);
		Test.TestTrue(FString::Printf(TEXT("%s: the refusal names '%s' [%s]"), Label, Name, *Error.ErrorMessage), Error.ErrorMessage.Contains(Name));
		Test.TestEqual(FString::Printf(TEXT("%s: nothing changed"), Label), StructureDigest(Impl.Blueprint), Before);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationReplaceInterfaceImplementedTest,
	"Cortex.Graph.Authoring.Migration.ReplaceInterface.Implemented",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationReplaceInterfaceImplementedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphReplaceInterfaceTest;
	using namespace CortexGraphReplaceInterfaceFixTest;

	FScope Scope;
	// A parameter passthrough: entry data output -> result data input, no other node.
	{
		UBlueprint* const Echo = MakeInterface(Scope, TEXT("BPI_Issue122Echo"), TEXT("Echo"), TEXT("Notify"), nullptr, true);
		FImplementer Impl;
		if (TestNotNull(TEXT("Echo BPI"), Echo)
			&& TestTrue(TEXT("Echo implementer"), MakeImplementer(Scope, Echo, TEXT("BP_Issue122Echo"), Impl, nullptr, false, TEXT("/Game/Temp"), TEXT("Echo"))))
		{
			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			UEdGraphPin* const In = Impl.FunctionEntry->FindPin(TEXT("In"), EGPD_Output);
			UEdGraphPin* const Value = Impl.FunctionResult->FindPin(TEXT("Value"), EGPD_Input);
			if (TestTrue(TEXT("The passthrough is linked"), In && Value && Schema->TryCreateConnection(In, Value)))
			{
				ExpectOmissionRefused(*this, Echo, Impl, TEXT("Echo"), TEXT("Passthrough"));
			}
		}
	}
	// An authored constant return.
	{
		UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122Const"));
		FImplementer Impl;
		if (TestNotNull(TEXT("Const BPI"), Interface)
			&& TestTrue(TEXT("Const implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Const"), Impl, nullptr, false)))
		{
			UEdGraphPin* const Value = Impl.FunctionResult->FindPin(TEXT("Value"), EGPD_Input);
			if (TestNotNull(TEXT("The result input"), Value))
			{
				GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*Value, TEXT("42.0"));
				ExpectOmissionRefused(*this, Interface, Impl, TEXT("GetValue"), TEXT("Constant"));
			}
		}
	}
	// A genuine engine-made empty stub stays droppable.
	{
		UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122Stub"));
		FImplementer Impl;
		if (TestNotNull(TEXT("Stub BPI"), Interface)
			&& TestTrue(TEXT("Stub implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Stub"), Impl, nullptr, false)))
		{
			FCortexGraphPreparedPatch Prepared;
			FCortexCommandResult Error;
			TestTrue(FString::Printf(TEXT("An empty stub may be omitted: %s"), *Error.ErrorMessage),
				FCortexGraphPatchOps::Preflight(Impl.Blueprint,
					Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122502"), MigrationJson(Interface->GetPathName(), NativePath(), OnlyNotify())), Prepared, Error));
			FCortexGraphMigrationInterfacePlan Plan;
			FCortexCommandResult PlanError;
			FCortexGraphMigrationInterfacePlan::FromJson(Prepared.InterfacePlan, Plan, PlanError);
			TestTrue(TEXT("... and is listed as a dropped stub"), Plan.DroppedStubs.Num() == 1 && Plan.DroppedStubs[0].Function == TEXT("GetValue"));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationReplaceInterfaceResultDefaultTest,
	"Cortex.Graph.Authoring.Migration.ReplaceInterface.ResultDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationReplaceInterfaceResultDefaultTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphReplaceInterfaceTest;
	using namespace CortexGraphReplaceInterfaceFixTest;

	FScope Scope;
	UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122Default"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("BPI"), Interface) || !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Default"), Impl)))
	{
		return false;
	}
	UEdGraphPin* const Literal = Impl.FunctionResult->FindPin(TEXT("Value"), EGPD_Input);
	if (!TestNotNull(TEXT("The result input"), Literal))
	{
		return false;
	}
	GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*Literal, TEXT("42.0"));
	const FString StructureBefore = StructureDigest(Impl.Blueprint);
	const FString OldPath = Interface->GetPathName();

	// A failed apply restores the literal.
	{
		TSharedPtr<FJsonObject> FaultRequest = Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122503"), MigrationJson(OldPath, NativePath(), FullFunctionMap()));
		FCortexCommandResult PreviewError;
		if (TestTrue(TEXT("The fault preview succeeds"), PreviewForApply(Impl.Blueprint, FaultRequest, PreviewError)))
		{
			FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("migration_interface_after_entries"));
			FCortexGraphPatchOutcome Outcome;
			FCortexCommandResult Error;
			FCompileCounter Counter;
			TestFalse(TEXT("The fault fails the apply"), FCortexGraphPatchOps::Execute(Impl.Blueprint, FaultRequest, Outcome, Error));
			FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
			TestEqual(TEXT("restored"), Outcome.RollbackStatus, FString(TEXT("restored")));
			TestEqual(TEXT("one recovery compile"), Counter.RecoveryCompiles, 1);
			TestEqual(TEXT("exact pre-state"), StructureDigest(Impl.Blueprint), StructureBefore);
			UEdGraphNode* const Result = NodeIn(Impl.FunctionGraph, Impl.FunctionResultGuid);
			const UEdGraphPin* const Restored = Result ? Result->FindPin(TEXT("Value"), EGPD_Input) : nullptr;
			TestTrue(TEXT("The stale result carries its literal again"), Restored != nullptr && FCString::Atod(*Restored->DefaultValue) == 42.0);
		}
	}

	// The apply keeps the literal on the renamed replacement input.
	TSharedPtr<FJsonObject> ApplyRequest = Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122503"), MigrationJson(OldPath, NativePath(), FullFunctionMap()));
	FCortexCommandResult PreviewError;
	if (!TestTrue(TEXT("The preview binds the apply"), PreviewForApply(Impl.Blueprint, ApplyRequest, PreviewError)))
	{
		return false;
	}
	FCortexGraphPatchOutcome Outcome;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("The apply succeeds: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Impl.Blueprint, ApplyRequest, Outcome, Error));
	const FGuid* ResultGuid = Outcome.Locators.NodeGuidByClientId.Find(TEXT("result:GetValue"));
	UEdGraphNode* const NewResult = ResultGuid ? NodeIn(Impl.FunctionGraph, *ResultGuid) : nullptr;
	const UEdGraphPin* const Return = NewResult ? NewResult->FindPin(TEXT("ReturnValue"), EGPD_Input) : nullptr;
	TestTrue(TEXT("The replacement input keeps the authored literal"), Return != nullptr && FCString::Atod(*Return->DefaultValue) == 42.0);

	// M1: the replay publishes the entry and the result identity.
	{
		FCortexGraphPreparedPatch Replay;
		FCortexCommandResult ReplayError;
		TestTrue(TEXT("The replay previews"), FCortexGraphPatchOps::Preflight(Impl.Blueprint,
			Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122503"), MigrationJson(OldPath, NativePath(), FullFunctionMap())), Replay, ReplayError));
		TestTrue(TEXT("The replay is recognised"), Replay.bFullyReused);
		TestTrue(TEXT("The replay still maps the result identity"), Replay.NodeGuidByClientId.Contains(TEXT("result:GetValue")) && Replay.NodeGuidByClientId.Contains(TEXT("entry:GetValue")));
		TestEqual(TEXT("... with the initial GUID"), Replay.NodeGuidByClientId.FindRef(TEXT("result:GetValue")), ResultGuid ? *ResultGuid : FGuid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationReplaceInterfaceEdgeSetTest,
	"Cortex.Graph.Authoring.Migration.ReplaceInterface.EdgeSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationReplaceInterfaceEdgeSetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphReplaceInterfaceTest;
	using namespace CortexGraphReplaceInterfaceFixTest;

	FScope Scope;
	UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122Edges"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("BPI"), Interface) || !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Edges"), Impl)))
	{
		return false;
	}
	const FString StructureBefore = StructureDigest(Impl.Blueprint);
	TSharedPtr<FJsonObject> ApplyRequest = Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122504"), MigrationJson(Interface->GetPathName(), NativePath(), FullFunctionMap()));
	FCortexCommandResult PreviewError;
	if (!TestTrue(TEXT("The preview binds the apply"), PreviewForApply(Impl.Blueprint, ApplyRequest, PreviewError)))
	{
		return false;
	}
	// Every planned edge stays; one unplanned replacement-terminator -> body edge is added before the readback.
	const FGuid SecondGuid = Impl.EventSecondGuid;
	FCortexGraphPatchOps::SetPreReadbackMutatorForTesting([SecondGuid](UBlueprint* Blueprint)
	{
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			UEdGraphNode* Second = nullptr;
			UK2Node_Event* Event = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node->NodeGuid == SecondGuid) { Second = Node; }
				if (UK2Node_Event* Candidate = Cast<UK2Node_Event>(Node)) { if (Candidate->EventReference.GetMemberName() == TEXT("Notify")) { Event = Candidate; } }
			}
			if (Second && Event)
			{
				Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)->MakeLinkTo(Second->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
			}
		}
	});
	FCortexGraphPatchOutcome Outcome;
	FCortexCommandResult Error;
	{
		FCompileCounter Counter;
		TestFalse(TEXT("An extra replacement edge fails the readback"), FCortexGraphPatchOps::Execute(Impl.Blueprint, ApplyRequest, Outcome, Error));
		FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();
		TestEqual(TEXT("mismatched"), Outcome.ReadbackStatus, FString(TEXT("mismatched")));
		TestEqual(TEXT("restored"), Outcome.RollbackStatus, FString(TEXT("restored")));
		TestEqual(TEXT("one recovery compile"), Counter.RecoveryCompiles, 1);
	}
	TestEqual(TEXT("exact pre-state"), StructureDigest(Impl.Blueprint), StructureBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationReplaceInterfaceInventoryTest,
	"Cortex.Graph.Authoring.Migration.ReplaceInterface.Inventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationReplaceInterfaceInventoryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphReplaceInterfaceTest;

	FScope Scope;
	UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122Inventory"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("BPI"), Interface) || !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Inventory"), Impl)))
	{
		return false;
	}
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	if (!TestTrue(TEXT("The preview succeeds"), FCortexGraphPatchOps::Preflight(Impl.Blueprint,
		Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122505"), MigrationJson(Interface->GetPathName(), NativePath(), FullFunctionMap())), Prepared, Error)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Inventory = FCortexGraphInterfaceMigration::MakeInventory(Prepared.InterfacePlan);
	const TArray<TSharedPtr<FJsonValue>>* Preservation = nullptr;
	if (TestTrue(TEXT("The inventory publishes the surviving-node capture"), Inventory.IsValid() && Inventory->TryGetArrayField(TEXT("preservation"), Preservation) && Preservation->Num() == 2))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Preservation)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			double Surviving = -1;
			TestTrue(TEXT("Each graph carries its contract: graph, excluded nodes, surviving count and digest"),
				Entry.IsValid() && Entry->HasField(TEXT("graph_guid")) && Entry->HasField(TEXT("excluded_guids")) && Entry->HasField(TEXT("capture_digest"))
				&& Entry->TryGetNumberField(TEXT("surviving_nodes"), Surviving) && Surviving >= 1);
		}
	}
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
