// replace_interface apply, readback and journaled recovery (#122 Task 8). The old graphs are adopted by the new interface
// description, so every body node keeps its GUID; a failure at any phase restores the interface list, graph names and
// InterfaceGuids exactly with one recovery compile.

#include "Misc/AutomationTest.h"

#include "CortexAssetMutationGuard.h"
#include "CortexGraphReplaceInterfaceTestSupport.h"
#include "CortexGraphTestContentRoot.h"
#include "HAL/FileManager.h"
#include "Operations/CortexGraphInterfaceMigration.h"
#include "Operations/CortexGraphMigrationOps.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphReplaceInterfaceApplyTest
{
	using namespace CortexGraphReplaceInterfaceTest;

	/** Clears every injected coordinator fault so no case can leak into the next one. */
	void ClearFaults()
	{
		FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
		FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);
		FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();
	}

	/** True when a node of the Blueprint still belongs to the interface: an event or a function entry of it. */
	bool ReferencesInterface(UBlueprint* Blueprint, const UClass* Interface)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
				{
					const UClass* Owner = Event->EventReference.GetMemberParentClass(Event->GetBlueprintClassFromNode());
					if (Owner != nullptr && Owner->GetAuthoritativeClass() == Interface->GetAuthoritativeClass()) return true;
				}
				if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
				{
					const UClass* Owner = Entry->FunctionReference.GetMemberParentClass();
					if (Owner != nullptr && Owner->GetAuthoritativeClass() == Interface->GetAuthoritativeClass()) return true;
				}
			}
		}
		return false;
	}

	UEdGraphNode* NodeIn(UEdGraph* Graph, const FGuid& Guid)
	{
		return FCortexGraphMigrationOps::FindNodeByGuidInGraph(Graph, Guid);
	}

	/** The body of the fixture is intact: every body node resolves under its pre-apply GUID and the chains are linked. */
	void CheckBody(FAutomationTestBase& Test, const FImplementer& Impl, UEdGraphNode* NewFunctionEntry, UEdGraphNode* NewFunctionResult, UEdGraphNode* NewEvent, const TCHAR* Label)
	{
		Test.TestTrue(FString::Printf(TEXT("%s: the function body kept its GUID"), Label), NodeIn(Impl.FunctionGraph, Impl.FunctionBodyGuid) == Impl.FunctionBody);
		Test.TestTrue(FString::Printf(TEXT("%s: the event body kept its GUIDs"), Label),
			NodeIn(Impl.EventGraph, Impl.EventFirstGuid) == Impl.EventFirst && NodeIn(Impl.EventGraph, Impl.EventSecondGuid) == Impl.EventSecond);
		if (NewFunctionEntry != nullptr && NewFunctionResult != nullptr)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: entry.then -> body.execute"), Label),
				LinkedTo(NewFunctionEntry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Impl.FunctionBody, UEdGraphSchema_K2::PN_Execute));
			Test.TestTrue(FString::Printf(TEXT("%s: body.then_0 -> result.execute"), Label),
				LinkedTo(Impl.FunctionBody->FindPin(TEXT("then_0"), EGPD_Output), NewFunctionResult, UEdGraphSchema_K2::PN_Execute));
		}
		if (NewEvent != nullptr)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: event.then -> first.execute"), Label),
				LinkedTo(NewEvent->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Impl.EventFirst, UEdGraphSchema_K2::PN_Execute));
		}
		Test.TestTrue(FString::Printf(TEXT("%s: first.then_0 -> second.execute"), Label),
			LinkedTo(Impl.EventFirst->FindPin(TEXT("then_0"), EGPD_Output), Impl.EventSecond, UEdGraphSchema_K2::PN_Execute));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationReplaceInterfaceApplyTest,
	"Cortex.Graph.Authoring.Migration.ReplaceInterface.Apply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationReplaceInterfaceApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphReplaceInterfaceTest;
	using namespace CortexGraphReplaceInterfaceApplyTest;
	ClearFaults();

	FScope Scope;
	UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122Apply"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("BPI"), Interface) || !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Apply"), Impl)))
	{
		return false;
	}
	UClass* const Native = UCortexIssue122GraphInterface::StaticClass();
	UClass* const OldClass = Interface->GeneratedClass;
	const FString OldPath = Interface->GetPathName();
	const bool bInterfaceDirtyBefore = Interface->GetOutermost()->IsDirty();
	const FString StructureBefore = StructureDigest(Impl.Blueprint);
	const int32 FunctionGraphsBefore = Impl.Blueprint->FunctionGraphs.Num();
	const TCHAR* const PatchId = TEXT("00000000-0000-0000-0000-000000122201");

	// A stale expected_validation_hash is refused before anything moves.
	{
		TSharedPtr<FJsonObject> Stale = Request(Impl.Blueprint, PatchId, MigrationJson(OldPath, NativePath(), FullFunctionMap()), false);
		Stale->SetStringField(TEXT("expected_validation_hash"), TEXT("00000000000000000000000000000000"));
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		TestFalse(TEXT("A stale hash fails the apply"), FCortexGraphPatchOps::Execute(Impl.Blueprint, Stale, Outcome, Error));
		TestEqual(TEXT("... as STALE_PRECONDITION"), Error.ErrorCode, FString(CortexErrorCodes::StalePrecondition));
		TestEqual(TEXT("... without mutation"), StructureDigest(Impl.Blueprint), StructureBefore);
		TestEqual(TEXT("... without a compile"), Outcome.TargetCompileCount, 0);
	}

	TSharedPtr<FJsonObject> ApplyRequest = Request(Impl.Blueprint, PatchId, MigrationJson(OldPath, NativePath(), FullFunctionMap()));
	FCortexCommandResult PreviewError;
	if (!TestTrue(FString::Printf(TEXT("The preview binds the apply: %s"), *PreviewError.ErrorMessage), PreviewForApply(Impl.Blueprint, ApplyRequest, PreviewError)))
	{
		return false;
	}
	FCortexGraphPatchOutcome Outcome;
	FCortexCommandResult Error;
	bool bApplied = false;
	{
		FCompileCounter Counter;
		bApplied = FCortexGraphPatchOps::Execute(Impl.Blueprint, ApplyRequest, Outcome, Error);
		TestEqual(TEXT("Exactly one target compile"), Counter.TargetCompiles, 1);
		TestEqual(TEXT("No recovery compile"), Counter.RecoveryCompiles, 0);
	}
	if (!TestTrue(FString::Printf(TEXT("The apply succeeds: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))), bApplied))
	{
		return false;
	}
	TestEqual(TEXT("applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("compiled"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("no rollback"), Outcome.RollbackStatus, FString(TEXT("not_requested")));

	// The interface list: new present, old absent, the adopted graph owned by the new description.
	TestEqual(TEXT("The new interface is present once"), InterfaceCount(Impl.Blueprint, Native), 1);
	TestEqual(TEXT("The old interface is gone"), InterfaceCount(Impl.Blueprint, OldClass), 0);
	const FBPInterfaceDescription* const Description = FindDescription(Impl.Blueprint, Native);
	if (TestNotNull(TEXT("The new description"), Description) && TestEqual(TEXT("It owns exactly GetValue's graph"), Description->Graphs.Num(), 1))
	{
		TestTrue(TEXT("The adopted graph is the very same graph object"), Description->Graphs[0] == Impl.FunctionGraph);
		TestEqual(TEXT("... under the new function name"), Impl.FunctionGraph->GetName(), FString(TEXT("GetValue")));
		TestEqual(TEXT("... with the new InterfaceGuid (empty for a native interface)"), Impl.FunctionGraph->InterfaceGuid, FGuid());
		TestEqual(TEXT("... keeping its graph GUID"), Impl.FunctionGraph->GraphGuid, Impl.FunctionGraphGuid);
	}
	TestEqual(TEXT("Nothing leaked into FunctionGraphs"), Impl.Blueprint->FunctionGraphs.Num(), FunctionGraphsBefore);

	// Two mapped functions, two distinct deterministic identities per function.
	const FGuid* FunctionEntryGuid = Outcome.Locators.NodeGuidByClientId.Find(TEXT("entry:GetValue"));
	const FGuid* FunctionResultGuid = Outcome.Locators.NodeGuidByClientId.Find(TEXT("result:GetValue"));
	const FGuid* EventEntryGuid = Outcome.Locators.NodeGuidByClientId.Find(TEXT("entry:Notify"));
	UEdGraphNode* NewFunctionEntry = FunctionEntryGuid ? NodeIn(Impl.FunctionGraph, *FunctionEntryGuid) : nullptr;
	UEdGraphNode* NewFunctionResult = FunctionResultGuid ? NodeIn(Impl.FunctionGraph, *FunctionResultGuid) : nullptr;
	UEdGraphNode* NewEvent = EventEntryGuid ? NodeIn(Impl.EventGraph, *EventEntryGuid) : nullptr;
	TestTrue(TEXT("The two functions got distinct identities"),
		FunctionEntryGuid && EventEntryGuid && FunctionResultGuid && *FunctionEntryGuid != *EventEntryGuid && *FunctionEntryGuid != *FunctionResultGuid);
	if (TestNotNull(TEXT("New function entry"), NewFunctionEntry) && TestNotNull(TEXT("New function result"), NewFunctionResult) && TestNotNull(TEXT("New event"), NewEvent))
	{
		const UK2Node_FunctionEntry* const Entry = Cast<UK2Node_FunctionEntry>(NewFunctionEntry);
		TestTrue(TEXT("The entry is the native interface's"),
			Entry != nullptr && Entry->FunctionReference.GetMemberName() == TEXT("GetValue") && Entry->FunctionReference.GetMemberParentClass() == Native);
		const UEdGraphPin* const ReturnPin = NewFunctionResult->FindPin(TEXT("ReturnValue"), EGPD_Input);
		TestTrue(TEXT("The result pins equal the native signature (ReturnValue: double)"),
			ReturnPin != nullptr && ReturnPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real && ReturnPin->PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double
			&& NewFunctionResult->FindPin(TEXT("Value")) == nullptr);
		const UK2Node_Event* const Event = Cast<UK2Node_Event>(NewEvent);
		TestTrue(TEXT("The event is the native interface's"),
			Event != nullptr && Event->EventReference.GetMemberName() == TEXT("Notify")
			&& Event->EventReference.GetMemberParentClass(Event->GetBlueprintClassFromNode())->GetAuthoritativeClass() == Native);
		const UEdGraphPin* const AmountPin = NewEvent->FindPin(TEXT("Amount"), EGPD_Output);
		TestTrue(TEXT("The event pins equal the native signature (Amount: int)"), AmountPin != nullptr && AmountPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int);
	}
	CheckBody(*this, Impl, NewFunctionEntry, NewFunctionResult, NewEvent, TEXT("Applied"));
	TestNull(TEXT("The stale entry is gone"), FCortexGraphMigrationOps::FindNodeByGuid(Impl.Blueprint, Impl.FunctionEntryGuid));
	TestNull(TEXT("The stale result is gone"), FCortexGraphMigrationOps::FindNodeByGuid(Impl.Blueprint, Impl.FunctionResultGuid));
	TestNull(TEXT("The stale event is gone"), FCortexGraphMigrationOps::FindNodeByGuid(Impl.Blueprint, Impl.EventGuid));
	TestFalse(TEXT("Nothing of the old interface is left orphaned"), ReferencesInterface(Impl.Blueprint, OldClass));
	TestTrue(TEXT("The compiler status is up to date"), Impl.Blueprint->Status == BS_UpToDate || Impl.Blueprint->Status == BS_UpToDateWithWarnings);
	TestEqual(TEXT("The old interface's package was never dirtied"), Interface->GetOutermost()->IsDirty(), bInterfaceDirtyBefore);

	// Replay of an applied request: an unchanged no-op without a compile.
	{
		TSharedPtr<FJsonObject> Replay = Request(Impl.Blueprint, PatchId, MigrationJson(OldPath, NativePath(), FullFunctionMap()));
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult ReplayError;
		if (TestTrue(FString::Printf(TEXT("The replay previews: %s"), *ReplayError.ErrorMessage), FCortexGraphPatchOps::Preflight(Impl.Blueprint, Replay, Prepared, ReplayError)))
		{
			TestTrue(TEXT("The replay is recognised as already applied"), Prepared.bFullyReused && !Prepared.bChanged);
			Replay->SetBoolField(TEXT("dry_run"), false);
			Replay->SetStringField(TEXT("expected_validation_hash"), Prepared.ValidationHash);
			const FString DigestBefore = StructureDigest(Impl.Blueprint);
			FCompileCounter Counter;
			FCortexGraphPatchOutcome ReplayOutcome;
			TestTrue(TEXT("The replay succeeds"), FCortexGraphPatchOps::Execute(Impl.Blueprint, Replay, ReplayOutcome, ReplayError));
			TestEqual(TEXT("... unchanged"), ReplayOutcome.ApplyStatus, FString(TEXT("unchanged")));
			TestEqual(TEXT("... without a compile"), Counter.TargetCompiles, 0);
			TestEqual(TEXT("... without a change"), StructureDigest(Impl.Blueprint), DigestBefore);
		}
	}

	// A from != to rename: the graph is renamed in this asset only, and a taken name refuses.
	{
		UBlueprint* const Renamed = MakeInterface(Scope, TEXT("BPI_Issue122Rename"), TEXT("ReadValue"), TEXT("Notify"));
		FImplementer RenameImpl;
		if (TestNotNull(TEXT("Rename BPI"), Renamed)
			&& TestTrue(TEXT("Rename implementer"), MakeImplementer(Scope, Renamed, TEXT("BP_Issue122Rename"), RenameImpl, nullptr, true, TEXT("/Game/Temp"), TEXT("ReadValue"))))
		{
			const bool bDirtyBefore = Renamed->GetOutermost()->IsDirty();
			TArray<TSharedPtr<FJsonValue>> Map;
			Map.Add(MapEntry(TEXT("ReadValue"), TEXT("GetValue"), FunctionPinMap()));
			Map.Add(MapEntry(TEXT("Notify"), nullptr, EventPinMap()));
			TSharedPtr<FJsonObject> RenameRequest = Request(RenameImpl.Blueprint, TEXT("00000000-0000-0000-0000-000000122202"), MigrationJson(Renamed->GetPathName(), NativePath(), Map));
			FCortexCommandResult RenamePreview;
			if (TestTrue(FString::Printf(TEXT("The rename previews: %s"), *RenamePreview.ErrorMessage), PreviewForApply(RenameImpl.Blueprint, RenameRequest, RenamePreview)))
			{
				FCortexGraphPatchOutcome RenameOutcome;
				FCortexCommandResult RenameError;
				TestTrue(FString::Printf(TEXT("The rename applies: %s [%s]"), *RenameError.ErrorMessage, *FString::Join(RenameOutcome.Diagnostics, TEXT("; "))),
					FCortexGraphPatchOps::Execute(RenameImpl.Blueprint, RenameRequest, RenameOutcome, RenameError));
				TestEqual(TEXT("The renamed graph carries the new name"), RenameImpl.FunctionGraph->GetName(), FString(TEXT("GetValue")));
				TestNull(TEXT("The old name is free"), FindObject<UObject>(RenameImpl.Blueprint, TEXT("ReadValue")));
				TestEqual(TEXT("The graph keeps its identity"), RenameImpl.FunctionGraph->GraphGuid, RenameImpl.FunctionGraphGuid);
				TestTrue(TEXT("The rename dirtied no other package"), Renamed->GetOutermost()->IsDirty() == bDirtyBefore);
				TestTrue(TEXT("The body survived the rename"), NodeIn(RenameImpl.FunctionGraph, RenameImpl.FunctionBodyGuid) == RenameImpl.FunctionBody);
			}
		}
		UBlueprint* const Taken = MakeInterface(Scope, TEXT("BPI_Issue122Taken"), TEXT("ReadValue"), TEXT("Notify"));
		FImplementer TakenImpl;
		if (TestNotNull(TEXT("Taken BPI"), Taken)
			&& TestTrue(TEXT("Taken implementer"), MakeImplementer(Scope, Taken, TEXT("BP_Issue122Taken"), TakenImpl, nullptr, true, TEXT("/Game/Temp"), TEXT("ReadValue"))))
		{
			UEdGraph* const Occupant = FBlueprintEditorUtils::CreateNewGraph(TakenImpl.Blueprint, TEXT("GetValue"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(TakenImpl.Blueprint, Occupant, true, nullptr);
			TArray<TSharedPtr<FJsonValue>> Map;
			Map.Add(MapEntry(TEXT("ReadValue"), TEXT("GetValue"), FunctionPinMap()));
			Map.Add(MapEntry(TEXT("Notify"), nullptr, EventPinMap()));
			const FString Before = StructureDigest(TakenImpl.Blueprint);
			FCortexGraphPreparedPatch Prepared;
			FCortexCommandResult TakenError;
			TestFalse(TEXT("A taken name refuses"), FCortexGraphPatchOps::Preflight(TakenImpl.Blueprint,
				Request(TakenImpl.Blueprint, TEXT("00000000-0000-0000-0000-000000122203"), MigrationJson(Taken->GetPathName(), NativePath(), Map)), Prepared, TakenError));
			TestTrue(FString::Printf(TEXT("... naming the taken name [%s]"), *TakenError.ErrorMessage), TakenError.ErrorMessage.Contains(TEXT("already taken")));
			TestEqual(TEXT("... without mutation"), StructureDigest(TakenImpl.Blueprint), Before);
		}
	}

	// An unimplemented event stub that is left out of function_map is dropped and reported.
	{
		UBlueprint* const StubInterface = MakeInterface(Scope, TEXT("BPI_Issue122DropEvent"));
		FImplementer StubImpl;
		if (TestNotNull(TEXT("Stub BPI"), StubInterface) && TestTrue(TEXT("Stub implementer"), MakeImplementer(Scope, StubInterface, TEXT("BP_Issue122DropEvent"), StubImpl)))
		{
			StubImpl.Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)->BreakAllPinLinks();
			FKismetEditorUtilities::CompileBlueprint(StubImpl.Blueprint);
			TArray<TSharedPtr<FJsonValue>> Map;
			Map.Add(MapEntry(TEXT("GetValue"), nullptr, FunctionPinMap()));
			TSharedPtr<FJsonObject> StubRequest = Request(StubImpl.Blueprint, TEXT("00000000-0000-0000-0000-000000122204"), MigrationJson(StubInterface->GetPathName(), NativePath(), Map));
			FCortexGraphPreparedPatch StubPrepared;
			FCortexCommandResult StubError;
			if (TestTrue(FString::Printf(TEXT("The stub preview succeeds: %s"), *StubError.ErrorMessage), FCortexGraphPatchOps::Preflight(StubImpl.Blueprint, StubRequest, StubPrepared, StubError)))
			{
				FCortexGraphMigrationInterfacePlan StubPlan;
				FCortexCommandResult PlanError;
				FCortexGraphMigrationInterfacePlan::FromJson(StubPrepared.InterfacePlan, StubPlan, PlanError);
				TestTrue(TEXT("The unimplemented event is listed as a dropped stub"),
					StubPlan.DroppedStubs.Num() == 1 && StubPlan.DroppedStubs[0].Function == TEXT("Notify") && StubPlan.DroppedStubs[0].Kind == TEXT("event"));
				StubRequest->SetBoolField(TEXT("dry_run"), false);
				StubRequest->SetStringField(TEXT("expected_validation_hash"), StubPrepared.ValidationHash);
				FCortexGraphPatchOutcome StubOutcome;
				TestTrue(FString::Printf(TEXT("The stub apply succeeds: %s [%s]"), *StubError.ErrorMessage, *FString::Join(StubOutcome.Diagnostics, TEXT("; "))),
					FCortexGraphPatchOps::Execute(StubImpl.Blueprint, StubRequest, StubOutcome, StubError));
				TestNull(TEXT("The dropped event node is removed"), FCortexGraphMigrationOps::FindNodeByGuid(StubImpl.Blueprint, StubImpl.EventGuid));
				TestFalse(TEXT("Nothing of the old interface is left"), ReferencesInterface(StubImpl.Blueprint, StubInterface->GeneratedClass));
				TestEqual(TEXT("The old interface is gone"), InterfaceCount(StubImpl.Blueprint, StubInterface->GeneratedClass), 0);
			}
		}
	}

	ClearFaults();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationReplaceInterfaceRollbackTest,
	"Cortex.Graph.Authoring.Migration.ReplaceInterface.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationReplaceInterfaceRollbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphReplaceInterfaceTest;
	using namespace CortexGraphReplaceInterfaceApplyTest;
	ClearFaults();
	EnsureCortexGraphTestTempContentRoot();

	FScope Scope;
	// ReadValue (implemented, renamed to GetValue) and GetValue (an unimplemented stub the new interface re-declares) plus Notify.
	UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122Rollback"), TEXT("ReadValue"), TEXT("Notify"), TEXT("GetValue"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("BPI"), Interface)
		|| !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Rollback"), Impl, nullptr, true, TEXT("/Game/Temp"), TEXT("ReadValue"), TEXT("GetValue"))))
	{
		return false;
	}
	UClass* const OldClass = Interface->GeneratedClass;
	const FString OldPath = Interface->GetPathName();
	TArray<TSharedPtr<FJsonValue>> Map;
	Map.Add(MapEntry(TEXT("ReadValue"), TEXT("GetValue"), FunctionPinMap()));
	Map.Add(MapEntry(TEXT("Notify"), nullptr, EventPinMap()));
	const auto MakeMigration = [&]() { return MigrationJson(OldPath, NativePath(), Map); };

	// The pre-state, by identity: interface list by class path and graph GUID, graph names, InterfaceGuids, body GUIDs and links.
	const FString StructureBefore = StructureDigest(Impl.Blueprint);
	const bool bDirtyBefore = Impl.Blueprint->GetOutermost()->IsDirty();
	TArray<UEdGraph*> GraphsBefore;
	for (UEdGraph* Graph : Impl.Blueprint->ImplementedInterfaces[0].Graphs) { GraphsBefore.Add(Graph); }
	TArray<FString> GraphNamesBefore;
	for (UEdGraph* Graph : GraphsBefore) { GraphNamesBefore.Add(Graph->GetName()); }
	const int32 FunctionGraphsBefore = Impl.Blueprint->FunctionGraphs.Num();

	const auto CheckRestored = [&](const TCHAR* Label, const FCortexGraphPatchOutcome& Outcome, const FCompileCounter& Counter)
	{
		TestEqual(FString::Printf(TEXT("%s: restored [%s]"), Label, *FString::Join(Outcome.Diagnostics, TEXT("; "))), Outcome.RollbackStatus, FString(TEXT("restored")));
		TestEqual(FString::Printf(TEXT("%s: exactly one recovery compile"), Label), Counter.RecoveryCompiles, 1);
		TestFalse(FString::Printf(TEXT("%s: the asset is not blocked"), Label), Outcome.bBlocked);
		TestEqual(FString::Printf(TEXT("%s: interface list, graph names, GUIDs, InterfaceGuids and nodes are exactly the pre-state"), Label),
			StructureDigest(Impl.Blueprint), StructureBefore);
		TestEqual(FString::Printf(TEXT("%s: the old interface is implemented once"), Label), InterfaceCount(Impl.Blueprint, OldClass), 1);
		TestEqual(FString::Printf(TEXT("%s: the new interface is not implemented"), Label), InterfaceCount(Impl.Blueprint, UCortexIssue122GraphInterface::StaticClass()), 0);
		const FBPInterfaceDescription* Description = FindDescription(Impl.Blueprint, OldClass);
		if (TestNotNull(FString::Printf(TEXT("%s: the old description"), Label), Description) && TestEqual(FString::Printf(TEXT("%s: its graph count"), Label), Description->Graphs.Num(), GraphsBefore.Num()))
		{
			for (int32 Index = 0; Index < GraphsBefore.Num(); ++Index)
			{
				TestTrue(FString::Printf(TEXT("%s: graph %d is the same object under its old name"), Label, Index),
					Description->Graphs[Index] == GraphsBefore[Index] && Description->Graphs[Index]->GetName() == GraphNamesBefore[Index]);
			}
		}
		TestEqual(FString::Printf(TEXT("%s: FunctionGraphs untouched"), Label), Impl.Blueprint->FunctionGraphs.Num(), FunctionGraphsBefore);
		TestEqual(FString::Printf(TEXT("%s: the dirty flag is restored"), Label), Impl.Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);
		UEdGraphNode* Entry = NodeIn(Impl.FunctionGraph, Impl.FunctionEntryGuid);
		UEdGraphNode* Result = NodeIn(Impl.FunctionGraph, Impl.FunctionResultGuid);
		UEdGraphNode* Event = NodeIn(Impl.EventGraph, Impl.EventGuid);
		TestTrue(FString::Printf(TEXT("%s: the stale terminators are back under their GUIDs"), Label), Entry && Result && Event);
		CheckBody(*this, Impl, Entry, Result, Event, Label);
		TestTrue(FString::Printf(TEXT("%s: compiled without error"), Label), Impl.Blueprint->Status != BS_Error);
	};

	struct FFault { const TCHAR* Apply; const TCHAR* Readback; const TCHAR* Label; bool bMutate; };
	const FFault Faults[] = {
		{ TEXT("migration_interface_after_swap"), nullptr, TEXT("fault after the swap"), false },
		{ TEXT("migration_interface_after_entries"), nullptr, TEXT("fault after the entry replacements"), false },
		{ TEXT("migration_interface_after_removal"), nullptr, TEXT("fault after the old interface removal"), false },
		{ nullptr, TEXT("interface_after_swap"), TEXT("readback fault"), false },
		{ nullptr, nullptr, TEXT("native-state readback divergence"), true },
	};
	for (const FFault& Fault : Faults)
	{
		TSharedPtr<FJsonObject> FaultRequest = Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122301"), MakeMigration());
		FCortexCommandResult PreviewError;
		if (!TestTrue(FString::Printf(TEXT("%s: the preview binds the apply [%s]"), Fault.Label, *PreviewError.ErrorMessage), PreviewForApply(Impl.Blueprint, FaultRequest, PreviewError)))
		{
			ClearFaults();
			return false;
		}
		FCortexGraphPatchOps::SetApplyFaultPointForTesting(Fault.Apply ? FName(Fault.Apply) : NAME_None);
		FCortexGraphPatchOps::SetReadbackFaultForTesting(Fault.Readback ? FName(Fault.Readback) : NAME_None);
		if (Fault.bMutate)
		{
			FCortexGraphPatchOps::SetPreReadbackMutatorForTesting([](UBlueprint* Blueprint)
			{
				for (FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
				{
					for (UEdGraph* Graph : Description.Graphs)
					{
						if (Graph) { Graph->InterfaceGuid = FGuid::NewGuid(); }
					}
				}
			});
		}
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		{
			FCompileCounter Counter;
			TestFalse(FString::Printf(TEXT("%s: the apply fails"), Fault.Label), FCortexGraphPatchOps::Execute(Impl.Blueprint, FaultRequest, Outcome, Error));
			ClearFaults();
			CheckRestored(Fault.Label, Outcome, Counter);
		}
	}

	// The asset is coherent after five rollbacks: the same request now applies, adopting the graph under its new name.
	{
		TSharedPtr<FJsonObject> Final = Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122301"), MakeMigration());
		FCortexCommandResult PreviewError;
		if (TestTrue(FString::Printf(TEXT("The final preview succeeds: %s"), *PreviewError.ErrorMessage), PreviewForApply(Impl.Blueprint, Final, PreviewError)))
		{
			FCortexGraphPatchOutcome Outcome;
			FCortexCommandResult Error;
			FCompileCounter Counter;
			TestTrue(FString::Printf(TEXT("The final apply succeeds: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
				FCortexGraphPatchOps::Execute(Impl.Blueprint, Final, Outcome, Error));
			TestEqual(TEXT("One compile"), Counter.TargetCompiles, 1);
			const FBPInterfaceDescription* Description = FindDescription(Impl.Blueprint, UCortexIssue122GraphInterface::StaticClass());
			TestTrue(TEXT("The new description owns the renamed graph"),
				Description != nullptr && Description->Graphs.Num() == 1 && Description->Graphs[0] == Impl.FunctionGraph && Impl.FunctionGraph->GetName() == TEXT("GetValue"));
			TestEqual(TEXT("The old interface is gone"), InterfaceCount(Impl.Blueprint, OldClass), 0);
			TestTrue(TEXT("The body GUIDs survived"), NodeIn(Impl.FunctionGraph, Impl.FunctionBodyGuid) == Impl.FunctionBody);
		}
	}

	// An unverifiable recovery blocks the asset and a second apply is refused.
	{
		UBlueprint* const BlockInterface = MakeInterface(Scope, TEXT("BPI_Issue122Block"));
		FImplementer Block;
		if (TestNotNull(TEXT("Block BPI"), BlockInterface) && TestTrue(TEXT("Block implementer"), MakeImplementer(Scope, BlockInterface, TEXT("BP_Issue122Block"), Block)))
		{
			TSharedPtr<FJsonObject> BlockRequest = Request(Block.Blueprint, TEXT("00000000-0000-0000-0000-000000122302"),
				MigrationJson(BlockInterface->GetPathName(), NativePath(), FullFunctionMap()));
			FCortexCommandResult PreviewError;
			if (TestTrue(TEXT("The block preview succeeds"), PreviewForApply(Block.Blueprint, BlockRequest, PreviewError)))
			{
				FCortexGraphPatchOps::SetReadbackFaultForTesting(TEXT("interface_after_swap"));
				FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("verification_failure"));
				FCortexGraphPatchOutcome Outcome;
				FCortexCommandResult Error;
				TestFalse(TEXT("The apply fails"), FCortexGraphPatchOps::Execute(Block.Blueprint, BlockRequest, Outcome, Error));
				ClearFaults();
				TestEqual(TEXT("The recovery is unverified"), Outcome.RollbackStatus, FString(TEXT("unverified")));
				TestTrue(TEXT("The asset is blocked"), Outcome.bBlocked);
				FString Reason;
				TestTrue(TEXT("The mutation guard blocks the asset"), FCortexAssetMutationGuard::IsBlocked(Block.Blueprint, Reason));
				TSharedPtr<FJsonObject> Second = Request(Block.Blueprint, TEXT("00000000-0000-0000-0000-000000122302"),
					MigrationJson(BlockInterface->GetPathName(), NativePath(), FullFunctionMap()));
				Second->SetBoolField(TEXT("dry_run"), false);
				Second->SetStringField(TEXT("expected_validation_hash"), TEXT("00000000000000000000000000000000"));
				FCortexGraphPatchOutcome SecondOutcome;
				FCortexCommandResult SecondError;
				TestFalse(TEXT("A second apply is refused"), FCortexGraphPatchOps::Execute(Block.Blueprint, Second, SecondOutcome, SecondError));
			}
		}
	}

	// save=true on a dirty package is refused.
	{
		UBlueprint* const DirtyInterface = MakeInterface(Scope, TEXT("BPI_Issue122Dirty"));
		FImplementer Dirty;
		if (TestNotNull(TEXT("Dirty BPI"), DirtyInterface) && TestTrue(TEXT("Dirty implementer"), MakeImplementer(Scope, DirtyInterface, TEXT("BP_Issue122Dirty"), Dirty)))
		{
			Dirty.Blueprint->GetOutermost()->SetDirtyFlag(true);
			TSharedPtr<FJsonObject> DirtyRequest = Request(Dirty.Blueprint, TEXT("00000000-0000-0000-0000-000000122303"),
				MigrationJson(DirtyInterface->GetPathName(), NativePath(), FullFunctionMap()), false);
			DirtyRequest->SetBoolField(TEXT("save"), true);
			DirtyRequest->SetStringField(TEXT("expected_validation_hash"), TEXT("00000000000000000000000000000000"));
			const FString Before = StructureDigest(Dirty.Blueprint);
			FCortexGraphPatchOutcome Outcome;
			FCortexCommandResult Error;
			TestFalse(TEXT("save=true on a dirty package is refused"), FCortexGraphPatchOps::Execute(Dirty.Blueprint, DirtyRequest, Outcome, Error));
			TestEqual(TEXT("... as DIRTY_EDITOR_STATE"), Error.ErrorCode, FString(CortexErrorCodes::DirtyEditorState));
			TestEqual(TEXT("... without mutation"), StructureDigest(Dirty.Blueprint), Before);
		}
	}

	// save=true persists after readback, and a real reload reads back the same state.
	{
		UBlueprint* const SaveInterface = MakeInterface(Scope, TEXT("BPI_Issue122Save"));
		FImplementer Save;
		if (TestNotNull(TEXT("Save BPI"), SaveInterface)
			&& TestTrue(TEXT("Save implementer"), MakeImplementer(Scope, SaveInterface, TEXT("BP_Issue122Save"), Save, nullptr, true, TEXT("/Temp"))))
		{
			UPackage* const Package = Save.Blueprint->GetOutermost();
			const FString PackageName = Package->GetName();
			const FString ObjectName = Save.Blueprint->GetName();
			const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
			IFileManager::Get().Delete(*Filename, false, true, true);
			Package->SetDirtyFlag(false);
			TSharedPtr<FJsonObject> SaveRequest = Request(Save.Blueprint, TEXT("00000000-0000-0000-0000-000000122304"),
				MigrationJson(SaveInterface->GetPathName(), NativePath(), FullFunctionMap()));
			FCortexCommandResult PreviewError;
			if (TestTrue(FString::Printf(TEXT("The save preview succeeds: %s"), *PreviewError.ErrorMessage), PreviewForApply(Save.Blueprint, SaveRequest, PreviewError)))
			{
				SaveRequest->SetBoolField(TEXT("save"), true);
				FCortexGraphPatchOutcome Outcome;
				FCortexCommandResult Error;
				TestTrue(FString::Printf(TEXT("The save apply succeeds: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
					FCortexGraphPatchOps::Execute(Save.Blueprint, SaveRequest, Outcome, Error));
				TestEqual(TEXT("saved"), Outcome.SaveStatus, FString(TEXT("saved")));
				TestEqual(TEXT("post-save verified"), Outcome.PostSaveStatus, FString(TEXT("verified")));
				TestTrue(TEXT("The file exists"), IFileManager::Get().FileExists(*Filename));
				const FString Applied = StructureDigest(Save.Blueprint);

				// The replaced terminators are detached nodes nothing references any more; the undo record and the collector must
				// release them before a reload maps the old package onto the saved one.
				if (GEditor != nullptr && GEditor->Trans != nullptr)
				{
					GEditor->Trans->Reset(FText::FromString(TEXT("Release replace_interface save fixture")));
				}
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
				UBlueprint* const BeforeReload = Save.Blueprint;
				TArray<UPackage*> ToReload;
				ToReload.Add(Package);
				FText ReloadError;
				const bool bReloaded = UPackageTools::ReloadPackages(ToReload, ReloadError, EReloadPackagesInteractionMode::AssumeNegative);
				TestTrue(FString::Printf(TEXT("The engine reload succeeds: %s"), *ReloadError.ToString()), bReloaded);
				UPackage* const Reloaded = FindPackage(nullptr, *PackageName);
				UBlueprint* const ReloadedBlueprint = Reloaded ? FindObject<UBlueprint>(Reloaded, *ObjectName) : nullptr;
				if (TestNotNull(TEXT("The reloaded Blueprint"), ReloadedBlueprint))
				{
					TestTrue(TEXT("It is a different object"), ReloadedBlueprint != BeforeReload);
					TestEqual(TEXT("The reloaded state is the applied state"), StructureDigest(ReloadedBlueprint), Applied);
					TestEqual(TEXT("The reloaded Blueprint implements the new interface once"), InterfaceCount(ReloadedBlueprint, UCortexIssue122GraphInterface::StaticClass()), 1);
					TestEqual(TEXT("... and the old one not at all"), InterfaceCount(ReloadedBlueprint, SaveInterface->GeneratedClass), 0);
					const FBPInterfaceDescription* Description = FindDescription(ReloadedBlueprint, UCortexIssue122GraphInterface::StaticClass());
					TestTrue(TEXT("The reloaded graph keeps its GUID, name and InterfaceGuid"),
						Description != nullptr && Description->Graphs.Num() == 1 && Description->Graphs[0]->GraphGuid == Save.FunctionGraphGuid
						&& Description->Graphs[0]->GetName() == TEXT("GetValue") && Description->Graphs[0]->InterfaceGuid == FGuid());
					if (Description != nullptr && Description->Graphs.Num() == 1)
					{
						TestTrue(TEXT("The reloaded body kept its GUID"), NodeIn(Description->Graphs[0], Save.FunctionBodyGuid) != nullptr);
					}
				}
				if (Reloaded)
				{
					if (GEditor != nullptr && GEditor->Trans != nullptr)
					{
						GEditor->Trans->Reset(FText::FromString(TEXT("Retire replace_interface save fixture")));
					}
					Reloaded->ClearFlags(RF_Standalone);
					Reloaded->MarkAsGarbage();
					ResetLoaders(Reloaded);
				}
				Scope.Blueprints.Remove(BeforeReload);
				FlushAsyncLoading();
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
				TestTrue(TEXT("The save fixture file is removed"), IFileManager::Get().Delete(*Filename, false, true, true) || !IFileManager::Get().FileExists(*Filename));
			}
		}
	}

	ClearFaults();
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
