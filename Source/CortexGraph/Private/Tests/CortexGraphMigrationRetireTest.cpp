#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Misc/AutomationTest.h"

#include "WidgetBlueprint.h"
#include "CortexGraphMigrationTestTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_Composite.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Operations/CortexGraphMigrationOps.h"
#include "UObject/Package.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS
namespace CortexGraphMigrationRetireTest
{
struct FFixture
{
	UPackage* Package = nullptr;
	UWidgetBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	UK2Node_Event* Alpha = nullptr;
	UK2Node_Event* Beta = nullptr;
	UK2Node_Event* Retained = nullptr;
	UK2Node_CallFunction* Producer = nullptr;
	UK2Node_CallFunction* AlphaBody = nullptr;
	UK2Node_CallFunction* BetaBody = nullptr;
	UK2Node_CallFunction* RetainedBody = nullptr;

	UK2Node_Event* AddEvent(const TCHAR* Name)
	{
		UK2Node_Event* Event = NewObject<UK2Node_Event>(Graph);
		Event->EventReference.SetExternalMember(FName(Name), UCortexGraphRetireLegacyWidget::StaticClass());
		Event->bOverrideFunction = true;
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();
		Graph->AddNode(Event, true, false);
		return Event;
	}

	UK2Node_CallFunction* AddCall(UFunction* Function)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Call->FunctionReference.SetExternalMember(Function->GetFName(), Function->GetOuterUClass());
		Call->CreateNewGuid();
		Call->AllocateDefaultPins();
		Graph->AddNode(Call, true, false);
		return Call;
	}

	bool Build(const TCHAR* Name, const bool bRetainProducer = false, const bool bBlockAlpha = false,
		UClass* ParentClass = nullptr)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s"), Name));
		Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			ParentClass ? ParentClass : UCortexGraphRetireLegacyWidget::StaticClass(), Package, FName(Name), BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
		if (!Blueprint) return false;
		if (Blueprint->UbergraphPages.Num() == 0)
		{
			Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::GN_EventGraph,
				UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
		}
		else Graph = Blueprint->UbergraphPages[0];
		Alpha = AddEvent(TEXT("OnLegacyAlpha"));
		Beta = AddEvent(TEXT("OnLegacyBeta"));
		Retained = AddEvent(TEXT("OnRetainedEvent"));
		Producer = AddCall(UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_IntToString")));
		AlphaBody = AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		BetaBody = AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		RetainedBody = AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		const UEdGraphSchema* Schema = Graph->GetSchema();
		auto Link = [Schema](UEdGraphNode* From, const TCHAR* FromName, UEdGraphNode* To, const TCHAR* ToName)
		{
			UEdGraphPin* FromPin = From ? From->FindPin(FName(FromName)) : nullptr;
			UEdGraphPin* ToPin = To ? To->FindPin(FName(ToName)) : nullptr;
			return Schema && FromPin && ToPin && Schema->TryCreateConnection(FromPin, ToPin);
		};
		if (!Link(Alpha, TEXT("then"), AlphaBody, TEXT("execute"))
			|| !Link(Beta, TEXT("then"), BetaBody, TEXT("execute"))
			|| !Link(Producer, TEXT("ReturnValue"), AlphaBody, TEXT("InString"))
			|| !Link(Producer, TEXT("ReturnValue"), BetaBody, TEXT("InString"))) return false;
		if (bRetainProducer)
		{
			if (!Link(Retained, TEXT("then"), RetainedBody, TEXT("execute"))
				|| !Link(Producer, TEXT("ReturnValue"), RetainedBody, TEXT("InString"))) return false;
		}
		if (bBlockAlpha)
		{
			UK2Node_CallFunction* RetainedData = AddCall(UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_IntToString")));
			if (!Link(Retained, TEXT("then"), RetainedBody, TEXT("execute"))
				|| !Link(Alpha, TEXT("Value"), RetainedData, TEXT("InInt"))
				|| !Link(RetainedData, TEXT("ReturnValue"), RetainedBody, TEXT("InString"))) return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> Migration(const TArray<FString>& Entries, const bool bApproved = false,
		const TArray<FString>& Approved = {}) const
	{
		TSharedPtr<FJsonObject> MigrationJson = MakeShared<FJsonObject>();
		MigrationJson->SetStringField(TEXT("op"), TEXT("retire_entries"));
		TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
		GraphRef->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
		Source->SetObjectField(TEXT("graph_ref"), GraphRef);
		TArray<TSharedPtr<FJsonValue>> EntryValues;
		for (const FString& Entry : Entries) EntryValues.Add(MakeShared<FJsonValueString>(Entry));
		Source->SetArrayField(TEXT("entry_node_guids"), EntryValues);
		MigrationJson->SetObjectField(TEXT("source"), Source);
		if (bApproved)
		{
			TArray<TSharedPtr<FJsonValue>> ApprovedValues;
			for (const FString& Entry : Approved) ApprovedValues.Add(MakeShared<FJsonValueString>(Entry));
			MigrationJson->SetArrayField(TEXT("approved_node_guids"), ApprovedValues);
		}
		return MigrationJson;
	}

	void Cleanup()
	{
		if (Blueprint) { Blueprint->ClearFlags(RF_Standalone); Blueprint->MarkAsGarbage(); Blueprint = nullptr; }
		if (Package) { Package->ClearFlags(RF_Standalone); Package->MarkAsGarbage(); Package = nullptr; }
	}
};

bool Plan(FFixture& Fixture, const TArray<FString>& Entries, FCortexGraphMigrationRetirePlan& OutPlan,
	bool& bReused, FCortexCommandResult& Error, const bool bApproved = false, const TArray<FString>& Approved = {})
{
	return FCortexGraphMigrationOps::PlanRetirement(Fixture.Blueprint,
		Fixture.Migration(Entries, bApproved, Approved), OutPlan, bReused, Error);
}
FString CaptureNativeGraph(UEdGraph* Graph, const TSet<FGuid>* Filter = nullptr)
{
	TArray<FString> Records;
	if (!Graph) return FString();
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		const FString Guid = Node->NodeGuid.ToString();
		if (Filter && !Filter->Contains(Node->NodeGuid)) continue;
		Records.Add(FString::Printf(TEXT("N|%s|%s|%d|%d|%s"), *Guid, *Node->GetClass()->GetPathName(),
			Node->NodePosX, Node->NodePosY, *Node->NodeComment));
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			Records.Add(FString::Printf(TEXT("P|%s|%s|%d|%s|%s|%s"), *Guid, *Pin->PinName.ToString(),
				static_cast<int32>(Pin->Direction), *Pin->PinType.PinCategory.ToString(), *Pin->DefaultValue,
				Pin->DefaultObject ? *Pin->DefaultObject->GetPathName() : TEXT("")));
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				const UEdGraphNode* FarNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!FarNode || !LinkedPin || (Filter && !Filter->Contains(FarNode->NodeGuid))) continue;
				const FString From = Guid + TEXT(".") + Pin->PinName.ToString();
				const FString To = FarNode->NodeGuid.ToString() + TEXT(".") + LinkedPin->PinName.ToString();
				if (From < To) Records.Add(TEXT("L|") + From + TEXT("|") + To);
			}
		}
	}
	Records.Sort();
	return FString::Join(Records, TEXT("\n"));
}

bool PrepareApprovedRequest(
	FFixture& Fixture,
	const TCHAR* PatchId,
	TSharedPtr<FJsonObject>& OutRequest,
	TArray<FString>& OutApproved,
	FCortexCommandResult& OutError)
{
	OutRequest = MakeShared<FJsonObject>();
	OutRequest->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	OutRequest->SetStringField(TEXT("patch_id"), PatchId);
	OutRequest->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	OutRequest->SetArrayField(TEXT("nodes"), {});
	OutRequest->SetArrayField(TEXT("connections"), {});
	OutRequest->SetArrayField(TEXT("pin_updates"), {});
	OutRequest->SetBoolField(TEXT("dry_run"), true);
	OutRequest->SetBoolField(TEXT("compile"), false);
	OutRequest->SetBoolField(TEXT("save"), false);
	OutRequest->SetBoolField(TEXT("allow_noop"), false);
	TSharedPtr<FJsonObject> Migration = Fixture.Migration({
		Fixture.Alpha->NodeGuid.ToString(), Fixture.Beta->NodeGuid.ToString() });
	OutRequest->SetObjectField(TEXT("migration"), Migration);

	FCortexGraphPreparedPatch Preview;
	if (!FCortexGraphPatchOps::Preflight(Fixture.Blueprint, OutRequest, Preview, OutError)) return false;
	const TSharedPtr<FJsonObject> Inventory =
		FCortexGraphMigrationOps::MakeRetirementInventory(Preview.RetirementPlan);
	const TArray<TSharedPtr<FJsonValue>>* Removable = nullptr;
	if (!Inventory.IsValid() || !Inventory->TryGetArrayField(TEXT("removable"), Removable) || !Removable)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("retirement preview did not publish its removable set"));
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Removable) OutApproved.Add(Value->AsString());
	TArray<TSharedPtr<FJsonValue>> ApprovalValues;
	for (const FString& Guid : OutApproved) ApprovalValues.Add(MakeShared<FJsonValueString>(Guid));
	Migration->SetArrayField(TEXT("approved_node_guids"), ApprovalValues);

	FCortexGraphPreparedPatch Reviewed;
	if (!FCortexGraphPatchOps::Preflight(Fixture.Blueprint, OutRequest, Reviewed, OutError)) return false;
	OutRequest->SetBoolField(TEXT("dry_run"), false);
	OutRequest->SetStringField(TEXT("expected_validation_hash"), Reviewed.ValidationHash);
	return true;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireSelectedEntriesTest,
	"Cortex.Graph.Authoring.Migration.Retire.SelectedEntriesAndPrivateProducer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireSelectedEntriesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireSelected")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("retirement plan is computed: %s"), *Error.ErrorMessage),
		Plan(Fixture, { Fixture.Alpha->NodeGuid.ToString(), Fixture.Beta->NodeGuid.ToString() }, PlanValue, bReused, Error));
	TestTrue(TEXT("alpha is removable"), PlanValue.RemovableGuids.Contains(Fixture.Alpha->NodeGuid.ToString()));
	TestTrue(TEXT("beta is removable"), PlanValue.RemovableGuids.Contains(Fixture.Beta->NodeGuid.ToString()));
	TestTrue(TEXT("producer shared only by selected entries is removable"), PlanValue.RemovableGuids.Contains(Fixture.Producer->NodeGuid.ToString()));
	FCortexGraphMigrationRetirePlan RoundTrip;
	TestTrue(TEXT("durable plan round-trips"), FCortexGraphMigrationRetirePlan::FromJson(PlanValue.ToJson(), RoundTrip, Error));
	TestEqual(TEXT("round-trip keeps selected GUIDs"), RoundTrip.SelectedEntryGuids.Num(), 2);
	const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanValue.ToJson());
	TestNotNull(TEXT("retirement inventory is created"), Inventory.Get());
	if (Inventory.IsValid())
	{
		FString Status;
		TestTrue(TEXT("inventory reports explicit Blueprint status"), Inventory->TryGetStringField(TEXT("blueprint_status_before"), Status)
			&& Status.StartsWith(TEXT("BS_")));
		TestEqual(TEXT("inventory names cached diagnostic source"),
			Inventory->GetStringField(TEXT("preexisting_diagnostics_source")), FString(TEXT("cached_node_messages")));
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireStaleLegacyOverrideTest,
	"Cortex.Graph.Authoring.Migration.Retire.StaleLegacyOverrideAfterReparent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireStaleLegacyOverrideTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("reparented Widget fixture is created"), Fixture.Build(TEXT("BP_RetireStaleLegacy"),
		false, false, UCortexGraphRetireTargetWidget::StaticClass()));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	TestTrue(TEXT("the Widget BP is reparented away from the stale event parent"),
		!Fixture.Blueprint->ParentClass->IsChildOf(UCortexGraphRetireLegacyWidget::StaticClass()));
	TestEqual(TEXT("the event retains its legacy parent identity"),
		Fixture.Alpha->EventReference.GetMemberParentClass(), UCortexGraphRetireLegacyWidget::StaticClass());
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	TestTrue(FString::Printf(TEXT("stale legacy override remains eligible for retirement: %s"), *Error.ErrorMessage),
		Plan(Fixture, { AlphaGuid }, PlanValue, bReused, Error));
	TestTrue(TEXT("stale legacy override is removable"),
		PlanValue.RemovableGuids.Contains(AlphaGuid));
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireSharedProducerTest,
	"Cortex.Graph.Authoring.Migration.Retire.RetainsSharedProducer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireSharedProducerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireShared"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("retirement plan is computed: %s"), *Error.ErrorMessage),
		Plan(Fixture, { Fixture.Alpha->NodeGuid.ToString(), Fixture.Beta->NodeGuid.ToString() }, PlanValue, bReused, Error));
	TestTrue(TEXT("producer is reported shared"), PlanValue.Shared.ContainsByPredicate([&](const FCortexGraphPruneNode& Node)
		{ return Node.NodeGuid == Fixture.Producer->NodeGuid.ToString(); }));
	TestFalse(TEXT("shared producer is not removable"), PlanValue.RemovableGuids.Contains(Fixture.Producer->NodeGuid.ToString()));
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireBlockedEntryTest,
	"Cortex.Graph.Authoring.Migration.Retire.BlocksSelectedEntryFeedingRetainedLogic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireBlockedEntryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireBlocked"), false, true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	FCortexGraphMigrationRetirePlan PlanValue;

	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("partition-only preview includes blockers: %s"), *Error.ErrorMessage),
		Plan(Fixture, { AlphaGuid }, PlanValue, bReused, Error));
	TestTrue(TEXT("selected entry feeding retained logic is blocked"), PlanValue.Blocked.ContainsByPredicate(
		[&](const FCortexGraphPruneNode& Node) { return Node.NodeGuid == AlphaGuid; }));
	TestFalse(TEXT("blocked selected entry is not removable"), PlanValue.RemovableGuids.Contains(AlphaGuid));
	Error = FCortexCommandResult();
	TestFalse(TEXT("reviewed request refuses blockers"), Plan(Fixture, { AlphaGuid }, PlanValue, bReused, Error, true, { AlphaGuid }));
	TestTrue(TEXT("review refusal is diagnostic"), !Error.ErrorMessage.IsEmpty());
	Fixture.Cleanup();
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireStrictShapeTest,
	"Cortex.Graph.Authoring.Migration.Retire.StrictSourceAndEntryShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireStrictShapeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireStrict")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestFalse(TEXT("empty selected entry set is refused"),
		FCortexGraphMigrationOps::PlanRetirement(Fixture.Blueprint, Fixture.Migration({}), PlanValue, bReused, Error));
	Error = FCortexCommandResult();
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	TestFalse(TEXT("duplicate selected GUID is refused"),
		Plan(Fixture, { AlphaGuid, AlphaGuid }, PlanValue, bReused, Error));

	Error = FCortexCommandResult();
	TSharedPtr<FJsonObject> WrongGraph = Fixture.Migration({ AlphaGuid });
	const TSharedPtr<FJsonObject>* Source = nullptr;
	WrongGraph->TryGetObjectField(TEXT("source"), Source);
	const TSharedPtr<FJsonObject>* GraphRef = nullptr;
	(*Source)->TryGetObjectField(TEXT("graph_ref"), GraphRef);
	(*GraphRef)->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString());
	TestFalse(TEXT("wrong graph identity is refused"),
		FCortexGraphMigrationOps::PlanRetirement(Fixture.Blueprint, WrongGraph, PlanValue, bReused, Error));

	auto AddUnsupportedNode = [&Fixture](UEdGraphNode* Node)
	{
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		Fixture.Graph->AddNode(Node, true, false);
		return Node->NodeGuid.ToString();
	};
	Error = FCortexCommandResult();
	UK2Node_CustomEvent* Custom = NewObject<UK2Node_CustomEvent>(Fixture.Graph);
	Custom->CustomFunctionName = TEXT("OnLegacyAlpha");
	Custom->EventReference.SetExternalMember(TEXT("OnLegacyAlpha"), UCortexGraphRetireLegacyWidget::StaticClass());
	Custom->bOverrideFunction = true;
	const FString CustomGuid = AddUnsupportedNode(Custom);
	TestFalse(TEXT("override-shaped custom event is refused"),
		Plan(Fixture, { CustomGuid }, PlanValue, bReused, Error));

	Error = FCortexCommandResult();
	UK2Node_FunctionEntry* FunctionEntry = NewObject<UK2Node_FunctionEntry>(Fixture.Graph);
	const FString FunctionEntryGuid = AddUnsupportedNode(FunctionEntry);
	TestFalse(TEXT("function entry is refused"),
		Plan(Fixture, { FunctionEntryGuid }, PlanValue, bReused, Error));

	Error = FCortexCommandResult();
	UK2Node_FunctionResult* FunctionResult = NewObject<UK2Node_FunctionResult>(Fixture.Graph);
	const FString FunctionResultGuid = AddUnsupportedNode(FunctionResult);
	TestFalse(TEXT("function result is refused"),
		Plan(Fixture, { FunctionResultGuid }, PlanValue, bReused, Error));

	Error = FCortexCommandResult();
	UK2Node_CustomEvent* DuplicateOwner = NewObject<UK2Node_CustomEvent>(Fixture.Graph);
	DuplicateOwner->NodeGuid = Fixture.Alpha->NodeGuid;
	Fixture.Graph->AddNode(DuplicateOwner, true, false);
	TestFalse(TEXT("duplicate GUID ownership is refused"),
		Plan(Fixture, { AlphaGuid }, PlanValue, bReused, Error));

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireNestedGraphTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesNestedCompositeGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireNestedGraphTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireNested")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;

	UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(Fixture.Graph);
	Composite->CreateNewGuid();
	Fixture.Graph->AddNode(Composite, true, false);
	Composite->PostPlacedNewNode();
	UEdGraph* ChildGraph = Composite->BoundGraph;
	TestNotNull(TEXT("a real nested composite graph is created"), ChildGraph);
	if (!ChildGraph) { Fixture.Cleanup(); return false; }

	UK2Node_Event* ChildEntry = NewObject<UK2Node_Event>(ChildGraph);
	ChildEntry->EventReference.SetExternalMember(TEXT("OnLegacyAlpha"), UCortexGraphRetireLegacyWidget::StaticClass());
	ChildEntry->bOverrideFunction = true;
	ChildEntry->CreateNewGuid();
	ChildEntry->AllocateDefaultPins();
	ChildGraph->AddNode(ChildEntry, true, false);

	TSharedPtr<FJsonObject> Migration = Fixture.Migration({ ChildEntry->NodeGuid.ToString() });
	const TSharedPtr<FJsonObject>* Source = nullptr;
	Migration->TryGetObjectField(TEXT("source"), Source);
	const TSharedPtr<FJsonObject>* GraphRef = nullptr;
	(*Source)->TryGetObjectField(TEXT("graph_ref"), GraphRef);
	(*GraphRef)->SetStringField(TEXT("graph_guid"), ChildGraph->GraphGuid.ToString());
	TestFalse(TEXT("nested graph GUID with empty subgraph path is refused"),
		FCortexGraphMigrationOps::PlanRetirement(Fixture.Blueprint, Migration, PlanValue, bReused, Error));
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireUnsupportedBodyTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesBoundAndLatentBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireUnsupportedBodyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireUnsupportedBody")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	Fixture.Alpha->bInternalEvent = true;
	TestFalse(TEXT("bound/internal event is refused"),
		Plan(Fixture, { Fixture.Alpha->NodeGuid.ToString() }, PlanValue, bReused, Error));
	Fixture.Alpha->bInternalEvent = false;

	UK2Node_CallFunction* Delay = Fixture.AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("Delay")));
	const UEdGraphSchema* Schema = Fixture.Graph->GetSchema();
	TestTrue(TEXT("latent delay is attached to the selected body"),
		Schema->TryCreateConnection(Fixture.AlphaBody->FindPin(TEXT("then")), Delay->FindPin(TEXT("execute"))));
	TestTrue(TEXT("partition preview identifies latent body node"),
		Plan(Fixture, { Fixture.Alpha->NodeGuid.ToString() }, PlanValue, bReused, Error));
	TestTrue(TEXT("latent delay is blocked"), PlanValue.Blocked.ContainsByPredicate(
		[&](const FCortexGraphPruneNode& Node) { return Node.NodeGuid == Delay->NodeGuid.ToString(); }));
	Error = FCortexCommandResult();
	const TArray<FString> Approved = PlanValue.RemovableGuids;
	TestFalse(TEXT("reviewed retirement refuses the latent body blocker"),
		Plan(Fixture, { Fixture.Alpha->NodeGuid.ToString() }, PlanValue, bReused, Error, true, Approved));
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetirePatchEligibilityTest,
	"Cortex.Graph.Authoring.Migration.Retire.PatchEligibilityAndStrictPreflight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetirePatchEligibilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetirePatchEligibility")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	Fixture.Blueprint->Status = BS_Error;
	FCortexCommandResult Error;
	const TSharedPtr<FJsonObject> RetirementMigration = Fixture.Migration({ Fixture.Alpha->NodeGuid.ToString() });
	const TSharedPtr<FJsonObject> RetireRequest = MakeShared<FJsonObject>();
	RetireRequest->SetObjectField(TEXT("migration"), RetirementMigration);
	TestTrue(TEXT("retire_entries is eligible for a compiler-error Blueprint"),
		FCortexGraphPatchOps::ValidateEligibility(Fixture.Blueprint, RetireRequest, Error));
	Error = FCortexCommandResult();
	const TSharedPtr<FJsonObject> PruneRequest = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> PruneMigration = MakeShared<FJsonObject>();
	PruneMigration->SetStringField(TEXT("op"), TEXT("prune_island"));
	PruneRequest->SetObjectField(TEXT("migration"), PruneMigration);
	TestFalse(TEXT("prune_island remains ineligible for a compiler-error Blueprint"),
		FCortexGraphPatchOps::ValidateEligibility(Fixture.Blueprint, PruneRequest, Error));
	Error = FCortexCommandResult();
	const TSharedPtr<FJsonObject> AuthoringRequest = MakeShared<FJsonObject>();
	TestFalse(TEXT("ordinary authoring remains ineligible for a compiler-error Blueprint"),
		FCortexGraphPatchOps::ValidateEligibility(Fixture.Blueprint, AuthoringRequest, Error));

	TSharedPtr<FJsonObject> Malformed = MakeShared<FJsonObject>();
	Malformed->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	Malformed->SetStringField(TEXT("patch_id"), FGuid::NewGuid().ToString());
	Malformed->SetObjectField(TEXT("expected_fingerprint"),
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	Malformed->SetObjectField(TEXT("migration"), RetirementMigration);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
	Malformed->SetArrayField(TEXT("nodes"), Nodes);
	FCortexGraphPreparedPatch Prepared;
	Error = FCortexCommandResult();
	const FString BeforeHash = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
		->GetStringField(TEXT("graph_authoring_hash"));
	const int32 BeforeNodeCount = Fixture.Graph->Nodes.Num();
	TestFalse(TEXT("retirement with mixed authoring nodes is refused"),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Malformed, Prepared, Error));
	TestEqual(TEXT("mixed authoring fields report INVALID_FIELD"), Error.ErrorCode, FString(TEXT("INVALID_FIELD")));
	TestEqual(TEXT("malformed retirement does not change the graph hash"),
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)->GetStringField(TEXT("graph_authoring_hash")),
		BeforeHash);
	TestEqual(TEXT("malformed retirement does not change the graph node count"),
		Fixture.Graph->Nodes.Num(), BeforeNodeCount);
	Fixture.Cleanup();
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireFailureAtomicityTest,
	"Cortex.Graph.Authoring.Migration.Retire.FailureAtomicity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireFailureAtomicityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	struct FFaultCase
	{
		const TCHAR* Name;
		bool bReadback;
	};
	const FFaultCase Faults[] = {
		{ TEXT("migration_retire_after_first_removal"), false },
		{ TEXT("migration_retire_after_removals"), false },
		{ TEXT("retire_after_removal"), true },
		{ TEXT("retire_after_preservation"), true }
	};
	bool bAllPassed = true;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Faults); ++Index)
	{
		FFixture Fixture;
		const FString AssetName = FString::Printf(TEXT("BP_RetireAtomic_%d"), Index);
		bAllPassed &= TestTrue(TEXT("retirement fault fixture is created"), Fixture.Build(*AssetName, true));
		if (!Fixture.Blueprint) { Fixture.Cleanup(); continue; }
		const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
		const bool bDirtyBefore = Fixture.Package->IsDirty();
		TSharedPtr<FJsonObject> Request;
		TArray<FString> Approved;
		FCortexCommandResult Error;
		const FString PatchId = FString::Printf(TEXT("00000000-0000-0000-0000-00000010731%d"), Index);
		if (!PrepareApprovedRequest(Fixture, *PatchId, Request, Approved, Error))
		{
			bAllPassed &= TestFalse(FString::Printf(TEXT("%s approved preflight failed: %s"), Faults[Index].Name,
				*Error.ErrorMessage), true);
			Fixture.Cleanup();
			continue;
		}
		if (Faults[Index].bReadback)
		{
			FCortexGraphMigrationOps::SetRetirementReadbackFaultForTesting(FName(Faults[Index].Name));
		}
		else
		{
			FCortexGraphPatchOps::SetApplyFaultPointForTesting(FName(Faults[Index].Name));
		}
		FCortexGraphPatchOutcome Outcome;
		Error = FCortexCommandResult();
		bAllPassed &= TestFalse(FString::Printf(TEXT("%s causes Execute to fail"), Faults[Index].Name),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
		FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
		FCortexGraphMigrationOps::ClearRetirementReadbackFaultForTesting();
		bAllPassed &= TestEqual(FString::Printf(TEXT("%s restores the transaction"), Faults[Index].Name),
			Outcome.RollbackStatus, FString(TEXT("restored")));
		bAllPassed &= TestFalse(FString::Printf(TEXT("%s does not block the asset"), Faults[Index].Name),
			Outcome.bBlocked);
		if (Faults[Index].bReadback)
		{
			bAllPassed &= TestEqual(FString::Printf(TEXT("%s reports failed readback"), Faults[Index].Name),
				Outcome.ReadbackStatus, FString(TEXT("mismatched")));
		}
		bAllPassed &= TestEqual(FString::Printf(TEXT("%s restores the independent native graph snapshot"), Faults[Index].Name),
			CaptureNativeGraph(Fixture.Graph), GraphBefore);
		bAllPassed &= TestEqual(FString::Printf(TEXT("%s restores the dirty baseline"), Faults[Index].Name),
			Fixture.Package->IsDirty(), bDirtyBefore);
		for (const FString& GuidText : Approved)
		{
			FGuid Guid;
			FGuid::Parse(GuidText, Guid);
			bAllPassed &= TestNotNull(FString::Printf(TEXT("%s restores approved GUID %s"), Faults[Index].Name, *GuidText),
				FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
		}
		Fixture.Cleanup();
	}
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
	FCortexGraphMigrationOps::ClearRetirementReadbackFaultForTesting();
	return bAllPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireApplyReadbackTest,
	"Cortex.Graph.Authoring.Migration.Retire.ApplyReadbackWithoutCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireApplyReadbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture is created"), Fixture.Build(TEXT("BP_RetireApplyReadback"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-000000107301"));
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	Request->SetArrayField(TEXT("nodes"), {});
	Request->SetArrayField(TEXT("connections"), {});
	Request->SetArrayField(TEXT("pin_updates"), {});
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	const FString BetaGuid = Fixture.Beta->NodeGuid.ToString();
	TSharedPtr<FJsonObject> Migration = Fixture.Migration({ AlphaGuid, BetaGuid });
	Request->SetObjectField(TEXT("migration"), Migration);

	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("retirement preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Preview, Error));
	TArray<FString> Approved;
	const TSharedPtr<FJsonObject> PreviewInventory =
		FCortexGraphMigrationOps::MakeRetirementInventory(Preview.RetirementPlan);
	if (PreviewInventory.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ApprovedValues = nullptr;
		PreviewInventory->TryGetArrayField(TEXT("removable"), ApprovedValues);
		if (ApprovedValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ApprovedValues) Approved.Add(Value->AsString());
		}
	}
	TestTrue(TEXT("the reviewed approval includes both selected entries"), Approved.Contains(AlphaGuid) && Approved.Contains(BetaGuid));
	TArray<TSharedPtr<FJsonValue>> ApprovedValues;
	for (const FString& Guid : Approved) ApprovedValues.Add(MakeShared<FJsonValueString>(Guid));
	Migration->SetArrayField(TEXT("approved_node_guids"), ApprovedValues);
	FCortexGraphPreparedPatch Reviewed;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("approved preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Reviewed, Error));
	TSet<FGuid> RetainedGuids = { Fixture.Retained->NodeGuid, Fixture.Producer->NodeGuid, Fixture.RetainedBody->NodeGuid };
	const FString RetainedBefore = CaptureNativeGraph(Fixture.Graph, &RetainedGuids);
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Reviewed.ValidationHash);

	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("approved retirement applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("approved retirement reports applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
	for (const FString& GuidText : Approved)
	{
		FGuid Guid;
		FGuid::Parse(GuidText, Guid);
		TestNull(FString::Printf(TEXT("approved node %s is absent from the graph and asset"), *GuidText),
			FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
	}
	TestNotNull(TEXT("retained event remains"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.Retained->NodeGuid));
	TestNotNull(TEXT("shared producer remains"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.Producer->NodeGuid));
	TestNotNull(TEXT("retained body remains"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.RetainedBody->NodeGuid));
	TestEqual(TEXT("retained event, body and shared producer are unchanged"),
		CaptureNativeGraph(Fixture.Graph, &RetainedGuids), RetainedBefore);
	TestTrue(TEXT("retained body execute link survives"),
		Fixture.Retained->FindPin(TEXT("then"))->LinkedTo.Contains(Fixture.RetainedBody->FindPin(TEXT("execute"))));
	TestTrue(TEXT("shared producer remains linked to retained body"),
		Fixture.Producer->FindPin(TEXT("ReturnValue"))->LinkedTo.Contains(Fixture.RetainedBody->FindPin(TEXT("InString"))));
	TestEqual(TEXT("compile is not requested"), Outcome.CompileStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("native retirement readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("retirement requests no target compile"), Outcome.TargetCompileCount, 0);
	TestEqual(TEXT("rollback is not requested"), Outcome.RollbackStatus, FString(TEXT("not_requested")));
	TestTrue(TEXT("the package is dirty after retirement"), Fixture.Package->IsDirty());
	TestNotNull(TEXT("retirement inventory is returned"), Outcome.RetirementInventory.Get());
	const TArray<TSharedPtr<FJsonValue>>* InventoryApproved = nullptr;
	TestTrue(TEXT("inventory reports the exact applied approval"),
		Outcome.RetirementInventory.IsValid()
			&& Outcome.RetirementInventory->TryGetArrayField(TEXT("approved_guids"), InventoryApproved)
			&& InventoryApproved && InventoryApproved->Num() == Approved.Num());
	Fixture.Cleanup();
	return true;
}

#endif
