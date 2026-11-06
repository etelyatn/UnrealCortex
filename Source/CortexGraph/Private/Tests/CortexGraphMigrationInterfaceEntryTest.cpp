// replace_entry accepts interface-owned implementations (#122 Task 6): the function-style graph in
// FBPInterfaceDescription::Graphs and the event entry. The authoring shell still refuses them and names replace_interface.

#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexGraphInterfaceTestTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Operations/CortexGraphImplementationOps.h"
#include "Operations/CortexGraphMigrationOps.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "UObject/Package.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphInterfaceEntryTest
{
	struct FCompileCounter
	{
		int32 TargetCompiles = 0;
		int32 RecoveryCompiles = 0;
		static FCompileCounter* Active;

		void Begin()
		{
			Active = this;
			FCortexGraphPatchOps::SetOperationObserverForTesting(
				[](const FName Operation, UBlueprint*)
				{
					if (!Active) return;
					if (Operation == TEXT("target_compile")) ++Active->TargetCompiles;
					else if (Operation == TEXT("recovery_compile")) ++Active->RecoveryCompiles;
				});
		}

		void End()
		{
			FCortexGraphPatchOps::ClearOperationObserverForTesting();
			Active = nullptr;
		}
	};

	FCompileCounter* FCompileCounter::Active = nullptr;

	struct FFixture
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = nullptr;
		UClass* InterfaceClass = nullptr;

		UEdGraph* FunctionGraph = nullptr;
		UK2Node_FunctionEntry* FunctionEntry = nullptr;
		UK2Node_FunctionResult* FunctionResult = nullptr;
		UK2Node_ExecutionSequence* FunctionBody = nullptr;

		UEdGraph* EventGraph = nullptr;
		UK2Node_Event* Event = nullptr;
		UK2Node_ExecutionSequence* EventBody = nullptr;

		bool Create(const TCHAR* Name)
		{
			InterfaceClass = UCortexIssue122GraphInterface::StaticClass();
			Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s"), Name));
			Blueprint = FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(), Package, FName(Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			if (Blueprint == nullptr
				|| !FBlueprintEditorUtils::ImplementNewInterface(Blueprint, FTopLevelAssetPath(InterfaceClass))
				|| Blueprint->ImplementedInterfaces.Num() != 1 || Blueprint->ImplementedInterfaces[0].Graphs.Num() != 1)
			{
				return false;
			}
			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

			// Function-style: the engine-made entry/result pair of GetValue, with a body between them.
			FunctionGraph = Blueprint->ImplementedInterfaces[0].Graphs[0];
			for (UEdGraphNode* Node : FunctionGraph->Nodes)
			{
				if (FunctionEntry == nullptr) { FunctionEntry = Cast<UK2Node_FunctionEntry>(Node); }
				if (FunctionResult == nullptr) { FunctionResult = Cast<UK2Node_FunctionResult>(Node); }
			}
			if (FunctionEntry == nullptr || FunctionResult == nullptr)
			{
				return false;
			}
			UEdGraphPin* EntryThen = FunctionEntry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			EntryThen->BreakAllPinLinks();
			FunctionBody = NewObject<UK2Node_ExecutionSequence>(FunctionGraph);
			FunctionBody->CreateNewGuid();
			FunctionBody->AllocateDefaultPins();
			FunctionGraph->AddNode(FunctionBody, true, false);
			if (!Schema->TryCreateConnection(EntryThen, FunctionBody->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
				|| !Schema->TryCreateConnection(FunctionBody->FindPin(TEXT("then_0"), EGPD_Output), FunctionResult->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)))
			{
				return false;
			}

			// Event-style: Notify as an interface event with a body.
			EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
			if (EventGraph == nullptr)
			{
				return false;
			}
			Event = NewObject<UK2Node_Event>(EventGraph);
			Event->EventReference.SetExternalMember(TEXT("Notify"), InterfaceClass);
			Event->bOverrideFunction = true;
			Event->CreateNewGuid();
			Event->AllocateDefaultPins();
			EventGraph->AddNode(Event, true, false);
			EventBody = NewObject<UK2Node_ExecutionSequence>(EventGraph);
			EventBody->CreateNewGuid();
			EventBody->AllocateDefaultPins();
			EventGraph->AddNode(EventBody, true, false);
			if (!Schema->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), EventBody->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)))
			{
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			return Blueprint->Status != BS_Error;
		}

		void Cleanup()
		{
			if (GEditor && GEditor->Trans)
			{
				GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphInterfaceEntryTestCleanup")));
			}
			if (Blueprint)
			{
				if (Blueprint->GeneratedClass) { Blueprint->GeneratedClass->MarkAsGarbage(); }
				if (Blueprint->SkeletonGeneratedClass) { Blueprint->SkeletonGeneratedClass->MarkAsGarbage(); }
				Blueprint->ClearFlags(RF_Standalone);
				Blueprint->MarkAsGarbage();
				Blueprint = nullptr;
			}
			if (Package)
			{
				Package->ClearFlags(RF_Standalone);
				Package->MarkAsGarbage();
				Package = nullptr;
			}
		}
	};

	TSharedPtr<FJsonObject> PinMapEntry(const TCHAR* Entry, const TCHAR* Pin)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("entry"), Entry);
		Json->SetStringField(TEXT("from_pin"), Pin);
		Json->SetStringField(TEXT("to_pin"), Pin);
		return Json;
	}

	TArray<TSharedPtr<FJsonValue>> PinMap(std::initializer_list<TPair<const TCHAR*, const TCHAR*>> Pins)
	{
		TArray<TSharedPtr<FJsonValue>> Map;
		for (const TPair<const TCHAR*, const TCHAR*>& Pin : Pins)
		{
			Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(Pin.Key, Pin.Value)));
		}
		return Map;
	}

	TSharedPtr<FJsonObject> Target(UClass* InterfaceClass, const TCHAR* FunctionName)
	{
		TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
		Implementation->SetStringField(TEXT("owner_class"), InterfaceClass->GetPathName());
		Implementation->SetStringField(TEXT("function_name"), FunctionName);
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("implementation"), Implementation);
		return Result;
	}

	TSharedPtr<FJsonObject> BaseRequest(UBlueprint* Blueprint, const TCHAR* PatchId, const TSharedPtr<FJsonObject>& TargetJson)
	{
		TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Request->SetStringField(TEXT("patch_id"), PatchId);
		Request->SetObjectField(TEXT("target"), TargetJson);
		Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
		Request->SetBoolField(TEXT("dry_run"), true);
		Request->SetBoolField(TEXT("compile"), true);
		Request->SetBoolField(TEXT("save"), false);
		Request->SetBoolField(TEXT("allow_noop"), false);
		return Request;
	}

	TSharedPtr<FJsonObject> MigrationRequest(
		UBlueprint* Blueprint, const TCHAR* PatchId, UClass* InterfaceClass, const TCHAR* FunctionName,
		UEdGraph* SourceGraph, UEdGraphNode* SourceEntry, const TArray<TSharedPtr<FJsonValue>>& Map)
	{
		TSharedPtr<FJsonObject> Migration = MakeShared<FJsonObject>();
		Migration->SetStringField(TEXT("op"), TEXT("replace_entry"));
		TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
		GraphRef->SetStringField(TEXT("graph_guid"), SourceGraph->GraphGuid.ToString());
		Source->SetObjectField(TEXT("graph_ref"), GraphRef);
		Source->SetStringField(TEXT("entry_node_guid"), SourceEntry->NodeGuid.ToString());
		Migration->SetObjectField(TEXT("source"), Source);
		Migration->SetArrayField(TEXT("pin_map"), Map);
		TSharedPtr<FJsonObject> Request = BaseRequest(Blueprint, PatchId, Target(InterfaceClass, FunctionName));
		Request->SetObjectField(TEXT("migration"), Migration);
		return Request;
	}

	/** Preview, then bind the apply request to the preview's validation hash. */
	bool PreviewForApply(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Request, FCortexCommandResult& OutError)
	{
		FCortexGraphPreparedPatch Preview;
		if (!FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, OutError))
		{
			return false;
		}
		Request->SetBoolField(TEXT("dry_run"), false);
		Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
		return true;
	}

	bool LinkedTo(const UEdGraphPin* From, const UEdGraphNode* Node, const FName PinName)
	{
		const UEdGraphPin* To = Node ? Node->FindPin(PinName) : nullptr;
		return From && To && From->LinkedTo.Contains(To);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationInterfaceImplementationTest,
	"Cortex.Graph.Authoring.Migration.ReplaceEntry.InterfaceImplementation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationInterfaceImplementationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphInterfaceEntryTest;

	FFixture Fixture;
	if (!TestTrue(TEXT("Fixture implements the native interface with a function graph and an event"), Fixture.Create(TEXT("BP_Issue122InterfaceEntry"))))
	{
		Fixture.Cleanup();
		return false;
	}
	UClass* InterfaceClass = Fixture.InterfaceClass;
	UEdGraph* const InterfaceGraph = Fixture.FunctionGraph;
	const FGuid InterfaceGraphGuid = InterfaceGraph->GraphGuid;
	const FGuid StaleFunctionEntry = Fixture.FunctionEntry->NodeGuid;
	const FGuid StaleFunctionResult = Fixture.FunctionResult->NodeGuid;
	const FGuid FunctionBodyGuid = Fixture.FunctionBody->NodeGuid;
	const FGuid StaleEvent = Fixture.Event->NodeGuid;
	const FGuid EventBodyGuid = Fixture.EventBody->NodeGuid;
	const int32 InterfaceGraphNodesBefore = InterfaceGraph->Nodes.Num();
	const int32 FunctionGraphsBefore = Fixture.Blueprint->FunctionGraphs.Num();

	// The eligibility primitive: the migration flag accepts the interface, the default (authoring) call refuses it by name.
	{
		TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
		Selector->SetStringField(TEXT("owner_class"), InterfaceClass->GetPathName());
		Selector->SetStringField(TEXT("function_name"), TEXT("GetValue"));
		FCortexGraphImplementationPlan Plan;
		FCortexCommandResult Error;
		TestFalse(TEXT("The authoring eligibility refuses an interface function"),
			FCortexGraphImplementationOps::ValidateEligibility(Fixture.Blueprint, Selector, Plan, Error));
		TestEqual(TEXT("... as INVALID_OPERATION"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(FString::Printf(TEXT("... naming replace_interface [%s]"), *Error.ErrorMessage), Error.ErrorMessage.Contains(TEXT("replace_interface")));

		FCortexGraphImplementationPlan MigrationPlan;
		FCortexCommandResult MigrationError;
		TestTrue(FString::Printf(TEXT("The migration eligibility accepts it: %s"), *MigrationError.ErrorMessage),
			FCortexGraphImplementationOps::ValidateEligibility(Fixture.Blueprint, Selector, MigrationPlan, MigrationError, /*bAllowInterfaceImplementation=*/true));
		TestTrue(TEXT("The plan is flagged as an interface implementation"), MigrationPlan.bInterfaceImplementation);
		TestTrue(TEXT("The existing graph is the interface description's graph"), MigrationPlan.ExistingGraph == InterfaceGraph);
		TestTrue(TEXT("The existing entry is the engine-made entry"), MigrationPlan.ExistingEntryNode == Fixture.FunctionEntry);
	}

	// The authoring shell on the same target is still refused (Preflight is the shell's own entry point).
	{
		TSharedPtr<FJsonObject> Request = BaseRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000122001"), Target(InterfaceClass, TEXT("GetValue")));
		Request->SetArrayField(TEXT("nodes"), TArray<TSharedPtr<FJsonValue>>());
		Request->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
		Request->SetArrayField(TEXT("pin_updates"), TArray<TSharedPtr<FJsonValue>>());
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("The authoring shell refuses the interface target"), FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Prepared, Error));
		TestEqual(TEXT("... as INVALID_OPERATION"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(FString::Printf(TEXT("... naming replace_interface [%s]"), *Error.ErrorMessage), Error.ErrorMessage.Contains(TEXT("replace_interface")));
	}

	// 1. Function-style: the stale entry/result pair of GetValue is replaced and the body is preserved.
	{
		TSharedPtr<FJsonObject> Request = MigrationRequest(
			Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000122002"), InterfaceClass, TEXT("GetValue"),
			InterfaceGraph, Fixture.FunctionEntry,
			PinMap({{TEXT("output"), TEXT("then")}, {TEXT("input"), TEXT("execute")}, {TEXT("input"), TEXT("ReturnValue")}}));
		FCortexCommandResult Error;
		if (!TestTrue(FString::Printf(TEXT("Function-style preview succeeds: %s"), *Error.ErrorMessage), PreviewForApply(Fixture.Blueprint, Request, Error)))
		{
			Fixture.Cleanup();
			return false;
		}
		FCompileCounter Counter;
		Counter.Begin();
		FCortexGraphPatchOutcome Outcome;
		const bool bApplied = FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error);
		Counter.End();
		if (!TestTrue(FString::Printf(TEXT("Function-style replace_entry applies: %s"), *Error.ErrorMessage), bApplied))
		{
			Fixture.Cleanup();
			return false;
		}
		TestEqual(TEXT("applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
		TestEqual(TEXT("readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
		TestEqual(TEXT("one target compile"), Counter.TargetCompiles, 1);
		TestEqual(TEXT("no recovery compile"), Counter.RecoveryCompiles, 0);
		TestNull(TEXT("The stale entry is gone"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, StaleFunctionEntry));
		TestNull(TEXT("The stale result is gone"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, StaleFunctionResult));
		TestEqual(TEXT("The graph is still the interface description's graph"),
			Fixture.Blueprint->ImplementedInterfaces.Num() == 1 && Fixture.Blueprint->ImplementedInterfaces[0].Graphs.Num() == 1
				? Fixture.Blueprint->ImplementedInterfaces[0].Graphs[0]->GraphGuid : FGuid(),
			InterfaceGraphGuid);
		TestEqual(TEXT("Nothing leaked into FunctionGraphs"), Fixture.Blueprint->FunctionGraphs.Num(), FunctionGraphsBefore);
		TestEqual(TEXT("Same node count (entry and result replaced one for one)"), InterfaceGraph->Nodes.Num(), InterfaceGraphNodesBefore);

		const FGuid* EntryGuid = Outcome.Locators.NodeGuidByClientId.Find(TEXT("entry"));
		const FGuid* ResultGuid = Outcome.Locators.NodeGuidByClientId.Find(TEXT("result"));
		UEdGraphNode* NewEntry = EntryGuid ? FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, *EntryGuid) : nullptr;
		UEdGraphNode* NewResult = ResultGuid ? FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, *ResultGuid) : nullptr;
		if (TestNotNull(TEXT("New entry"), NewEntry) && TestNotNull(TEXT("New result"), NewResult))
		{
			TestTrue(TEXT("The new entry lives in the interface graph"), NewEntry->GetGraph() == InterfaceGraph);
			TestTrue(TEXT("The body node kept its identity"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, FunctionBodyGuid) == Fixture.FunctionBody);
			TestTrue(TEXT("entry.then -> body.execute is re-made"),
				LinkedTo(NewEntry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Fixture.FunctionBody, UEdGraphSchema_K2::PN_Execute));
			TestTrue(TEXT("body.then_0 -> result.execute is re-made"),
				LinkedTo(Fixture.FunctionBody->FindPin(TEXT("then_0"), EGPD_Output), NewResult, UEdGraphSchema_K2::PN_Execute));
		}
		TestTrue(TEXT("Compiled without error"), Fixture.Blueprint->Status != BS_Error);
	}

	// 2. Event-style: the stale Notify event is replaced and its body is preserved.
	{
		TSharedPtr<FJsonObject> Request = MigrationRequest(
			Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000122003"), InterfaceClass, TEXT("Notify"),
			Fixture.EventGraph, Fixture.Event,
			PinMap({{TEXT("output"), TEXT("OutputDelegate")}, {TEXT("output"), TEXT("then")}, {TEXT("output"), TEXT("Amount")}}));
		FCortexCommandResult Error;
		if (!TestTrue(FString::Printf(TEXT("Event preview succeeds: %s"), *Error.ErrorMessage), PreviewForApply(Fixture.Blueprint, Request, Error)))
		{
			Fixture.Cleanup();
			return false;
		}
		FCompileCounter Counter;
		Counter.Begin();
		FCortexGraphPatchOutcome Outcome;
		const bool bApplied = FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error);
		Counter.End();
		if (!TestTrue(FString::Printf(TEXT("Event replace_entry applies: %s"), *Error.ErrorMessage), bApplied))
		{
			Fixture.Cleanup();
			return false;
		}
		TestEqual(TEXT("applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
		TestEqual(TEXT("readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
		TestEqual(TEXT("one target compile"), Counter.TargetCompiles, 1);
		TestNull(TEXT("The stale event is gone"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, StaleEvent));
		const FGuid* EntryGuid = Outcome.Locators.NodeGuidByClientId.Find(TEXT("entry"));
		UEdGraphNode* NewEvent = EntryGuid ? FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, *EntryGuid) : nullptr;
		if (TestNotNull(TEXT("New event"), NewEvent))
		{
			const UK2Node_Event* NewEventNode = Cast<UK2Node_Event>(NewEvent);
			TestTrue(TEXT("The new entry is an event of the interface function"),
				NewEventNode && NewEventNode->EventReference.GetMemberName() == TEXT("Notify")
				&& NewEventNode->EventReference.GetMemberParentClass(NewEventNode->GetBlueprintClassFromNode()) == InterfaceClass);
			TestTrue(TEXT("The body node kept its identity"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, EventBodyGuid) == Fixture.EventBody);
			TestTrue(TEXT("event.then -> body.execute is re-made"),
				LinkedTo(NewEvent->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Fixture.EventBody, UEdGraphSchema_K2::PN_Execute));
		}
		TestTrue(TEXT("Compiled without error"), Fixture.Blueprint->Status != BS_Error);
	}

	Fixture.Cleanup();
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
