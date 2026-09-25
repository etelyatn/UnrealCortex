#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Misc/AutomationTest.h"

#include "WidgetBlueprint.h"
#include "CortexGraphMigrationTestTypes.h"
#include "CortexGraphTestContentRoot.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Editor/Transactor.h"
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
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "Dom/JsonObject.h"
#include "UObject/UObjectGlobals.h"

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
	FGuid AlphaGuid;
	FGuid BetaGuid;
	FGuid RetainedGuid;
	FGuid ProducerGuid;
	FGuid AlphaBodyGuid;
	FGuid BetaBodyGuid;
	FGuid RetainedBodyGuid;


	UK2Node_Event* AddEvent(const TCHAR* Name)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* Existing = Cast<UK2Node_Event>(Node);
			if (Existing && Existing->EventReference.GetMemberName() == FName(Name))
			{
				Existing->EventReference.SetExternalMember(FName(Name), UCortexGraphRetireLegacyWidget::StaticClass());
				Existing->bOverrideFunction = true;
				return Existing;
			}
		}
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
	UK2Node_CustomEvent* AddNativeNameCollision()
	{
		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
		Event->CustomFunctionName = TEXT("OnInitialized");
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();
		Graph->AddNode(Event, true, false);
		return Event;
	}


	bool Build(const TCHAR* Name, const bool bRetainProducer = false, const bool bBlockAlpha = false,
		UClass* ParentClass = nullptr, const bool bCompileParentFirst = false, const bool bDelayGraphNodes = false)
	{
		EnsureCortexGraphTestTempContentRoot();
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
		if (bCompileParentFirst) FKismetEditorUtilities::CompileBlueprint(Blueprint);
		return bDelayGraphNodes || PopulateGraph(bRetainProducer, bBlockAlpha);
	}

	bool PopulateGraph(const bool bRetainProducer = false, const bool bBlockAlpha = false)
	{
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
		AlphaGuid = Alpha->NodeGuid;
		BetaGuid = Beta->NodeGuid;
		RetainedGuid = Retained->NodeGuid;
		ProducerGuid = Producer->NodeGuid;
		AlphaBodyGuid = AlphaBody->NodeGuid;
		BetaBodyGuid = BetaBody->NodeGuid;
		RetainedBodyGuid = RetainedBody->NodeGuid;
		if (bBlockAlpha)
		{
			UK2Node_CallFunction* RetainedData = AddCall(UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_IntToString")));
			if (!Link(Retained, TEXT("then"), RetainedBody, TEXT("execute"))
				|| !Link(Alpha, TEXT("Value"), RetainedData, TEXT("InInt"))
				|| !Link(RetainedData, TEXT("ReturnValue"), RetainedBody, TEXT("InString"))) return false;
		}
		return true;
	}


	bool RefreshPointers()
	{
		auto Find = [this](const FGuid& Guid) -> UEdGraphNode*
		{
			for (UEdGraphNode* Node : Graph->Nodes) if (Node && Node->NodeGuid == Guid) return Node;
			return nullptr;
		};
		Alpha = Cast<UK2Node_Event>(Find(AlphaGuid));
		Beta = Cast<UK2Node_Event>(Find(BetaGuid));
		Retained = Cast<UK2Node_Event>(Find(RetainedGuid));
		Producer = Cast<UK2Node_CallFunction>(Find(ProducerGuid));
		AlphaBody = Cast<UK2Node_CallFunction>(Find(AlphaBodyGuid));
		BetaBody = Cast<UK2Node_CallFunction>(Find(BetaBodyGuid));
		RetainedBody = Cast<UK2Node_CallFunction>(Find(RetainedBodyGuid));
		return Alpha && Beta && Retained && Producer && AlphaBody && BetaBody && RetainedBody;
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

	FString Filename() const
	{
		return Package ? FPackageName::LongPackageNameToFilename(Package->GetName(),
			FPackageName::GetAssetPackageExtension()) : FString();
	}

	bool SaveToDisk()
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Blueprint, *Filename(), SaveArgs);
	}

	void Cleanup()
	{
		if (Blueprint) { Blueprint->ClearFlags(RF_Standalone); Blueprint->MarkAsGarbage(); Blueprint = nullptr; }
		if (Package) { Package->ClearFlags(RF_Standalone); Package->MarkAsGarbage(); Package = nullptr; }
	}
};

struct FOperations
{
	int32 TargetCompiles = 0;
	int32 RecoveryCompiles = 0;
	int32 Saves = 0;

	void Begin()
	{
		Active = this;
		FCortexGraphPatchOps::SetOperationObserverForTesting([](const FName Operation, UBlueprint*)
		{
			if (!Active) return;
			if (Operation == TEXT("target_compile")) ++Active->TargetCompiles;
			else if (Operation == TEXT("recovery_compile")) ++Active->RecoveryCompiles;
		});
		SaveHandle = UPackage::PackageSavedWithContextEvent.AddLambda(
			[](const FString&, UPackage*, FObjectPostSaveContext)
			{
				if (Active) ++Active->Saves;
			});
	}

	void End()
	{
		UPackage::PackageSavedWithContextEvent.Remove(SaveHandle);
		FCortexGraphPatchOps::ClearOperationObserverForTesting();
		Active = nullptr;
	}

private:
	static FOperations* Active;
	FDelegateHandle SaveHandle;
};

FOperations* FOperations::Active = nullptr;

TArray<uint8> ReadBytes(const FString& Filename)
{
	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *Filename);
	return Bytes;
}

bool SameBytes(const TArray<uint8>& Left, const TArray<uint8>& Right)
{
	return Left.Num() > 0 && Left.Num() == Right.Num()
		&& FMemory::Memcmp(Left.GetData(), Right.GetData(), Left.Num()) == 0;
}

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
	FCortexCommandResult& OutError,
	const bool bCompile = false,
	const bool bSave = false)
{
	OutRequest = MakeShared<FJsonObject>();
	OutRequest->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	OutRequest->SetStringField(TEXT("patch_id"), PatchId);
	OutRequest->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	OutRequest->SetArrayField(TEXT("nodes"), {});
	OutRequest->SetArrayField(TEXT("connections"), {});
	OutRequest->SetArrayField(TEXT("pin_updates"), {});
	OutRequest->SetBoolField(TEXT("dry_run"), true);
	OutRequest->SetBoolField(TEXT("compile"), bCompile);
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
	OutRequest->SetBoolField(TEXT("save"), bSave);
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireSharedExecutionBodyTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesSharedExecutionBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireSharedExecutionBodyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireSharedExecutionBody")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	const UEdGraphSchema* Schema = Fixture.Graph->GetSchema();
	UEdGraphPin* RetainedThen = Fixture.Retained->FindPin(TEXT("then"));
	UEdGraphPin* SharedBodyExecute = Fixture.AlphaBody->FindPin(TEXT("execute"));
	TestTrue(TEXT("retained entry can share the selected entry's execution body"),
		Schema && RetainedThen && SharedBodyExecute && Schema->TryCreateConnection(RetainedThen, SharedBodyExecute));
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);

	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	const FString SelectedGuid = Fixture.Alpha->NodeGuid.ToString();
	TestTrue(FString::Printf(TEXT("retirement preview succeeds: %s"), *Error.ErrorMessage),
		Plan(Fixture, { SelectedGuid }, PlanValue, bReused, Error));
	TestTrue(TEXT("execution body with an incoming retained execution edge is blocked"),
		PlanValue.Blocked.ContainsByPredicate([&](const FCortexGraphPruneNode& Node)
			{ return Node.NodeGuid == Fixture.AlphaBody->NodeGuid.ToString(); }));
	TestFalse(TEXT("blocked execution body is not approved for removal"),
		PlanValue.RemovableGuids.Contains(Fixture.AlphaBody->NodeGuid.ToString()));
	TestTrue(TEXT("selected entry feeding the retained body is blocked"),
		PlanValue.Blocked.ContainsByPredicate([&](const FCortexGraphPruneNode& Node)
			{ return Node.NodeGuid == SelectedGuid; }));
	TestFalse(TEXT("selected entry is not approved for removal"), PlanValue.RemovableGuids.Contains(SelectedGuid));

	Error = FCortexCommandResult();
	TestFalse(TEXT("reviewed retirement refuses the blocked shared execution body"),
		Plan(Fixture, { SelectedGuid }, PlanValue, bReused, Error, true, PlanValue.RemovableGuids));
	TestEqual(TEXT("shared execution body refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestEqual(TEXT("refused retirement leaves the complete graph unchanged"),
		CaptureNativeGraph(Fixture.Graph), GraphBefore);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireCompileRecoveryTest,
	"Cortex.Graph.Authoring.Migration.Retire.CompileRecoveryRejectsGeneratedDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireCompileRecoveryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("collision Widget fixture is created under its legacy parent"),
		Fixture.Build(TEXT("BP_RetireCompileRecovery"), false, false, nullptr, false, true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	IFileManager::Get().Delete(*Filename, false, true, true);
	TestTrue(TEXT("legacy-parent baseline is saved"), Fixture.SaveToDisk());
	Fixture.Blueprint->ParentClass = UCortexGraphRetireCollisionTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);
	Fixture.AddNativeNameCollision();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestEqual(TEXT("unrelated native collision establishes the original error status"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	TestTrue(TEXT("stale graph entries are created after failed compilation"), Fixture.PopulateGraph(true));
	Fixture.Retained->EventReference.SetExternalMember(TEXT("OnRetainedEvent"), UCortexGraphRetireTargetWidget::StaticClass());
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString GeneratedBefore = FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint);
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("approved compile request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107401"),
			Request, Approved, Error, true));
	FOperations Operations;
	Operations.Begin();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 2);
	AddExpectedError(TEXT("Pasted node"), EAutomationExpectedErrorFlags::Contains, 2);
	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestFalse(TEXT("retirement fails while unrelated compiler collision persists"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("generated state drift keeps recovery blocked"), Error.ErrorCode, FString(TEXT("INVALID_OPERATION")));
	TestEqual(TEXT("rollback refuses a changed generated class"), Outcome.RollbackStatus, FString(TEXT("unverified")));
	TestTrue(TEXT("unverified generated recovery blocks the asset"), Outcome.bBlocked);
	TestEqual(TEXT("the recovery compile retains BS_Error"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	TestNotEqual(TEXT("generated drift is not reported as exact restoration"),
		FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint), GeneratedBefore);
	TestNotEqual(TEXT("engine mutation during failed recovery is not represented as restored authoring"),
		CaptureNativeGraph(Fixture.Graph), GraphBefore);
	for (const FString& GuidText : Approved)
	{
		FGuid Guid;
		FGuid::Parse(GuidText, Guid);
		TestNotNull(TEXT("every approved GUID is restored"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
	}
	TestTrue(TEXT("blocked failed recovery remains dirty"), Fixture.Package->IsDirty());
	TestEqual(TEXT("failed apply does not save"), Operations.Saves, 0);
	Operations.End();
	Fixture.Cleanup();
	IFileManager::Get().Delete(*Filename, false, true, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireCompileRecoveryExactMatchTest,
	"Cortex.Graph.Authoring.Migration.Retire.CompileRecoveryAcceptsExactErrorState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireCompileRecoveryExactMatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("legacy Widget fixture includes the selected override entries"),
		Fixture.Build(TEXT("BP_RetireCompileRecoveryExact"), true, false, nullptr, false, false));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	IFileManager::Get().Delete(*Filename, false, true, true);
	Fixture.AddNativeNameCollision();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("legacy-parent entries and the unrelated compile error are saved"), Fixture.SaveToDisk());
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestEqual(TEXT("saved legacy-parent fixture has the real unrelated compile error"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	TestTrue(TEXT("legacy override nodes survive their valid-parent compile"), Fixture.RefreshPointers());

	Fixture.Blueprint->ParentClass = UCortexGraphRetireCollisionTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);
	TestTrue(TEXT("event nodes remain override nodes after reparent refresh"),
		Fixture.RefreshPointers() && Fixture.Alpha->bOverrideFunction && !Fixture.Alpha->IsA<UK2Node_CustomEvent>()
			&& Fixture.Beta->bOverrideFunction && !Fixture.Beta->IsA<UK2Node_CustomEvent>());
	FBPVariableDescription& CollisionVariable = Fixture.Blueprint->NewVariables.AddDefaulted_GetRef();
	CollisionVariable.VarName = FName(TEXT("NativeCollision"));
	CollisionVariable.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
	Fixture.Package->MarkPackageDirty();
	Fixture.Blueprint->Status = BS_Error;
	Fixture.Retained->EventReference.SetExternalMember(TEXT("OnRetainedEvent"), UCortexGraphRetireTargetWidget::StaticClass());

	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString GeneratedBefore = FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint);
	const bool bDirtyBefore = Fixture.Package->IsDirty();
	TestEqual(TEXT("the pre-request generated digest is captured exactly"),
		FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint), GeneratedBefore);
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("approved compile request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107405"),
			Request, Approved, Error, true));

	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("retirement_compile_result_failure"));
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("injected compile-result failure enters recovery"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	FCortexGraphPatchOps::ClearApplyFaultPointForTesting();

	TestEqual(TEXT("target compile fails once"), Outcome.TargetCompileCount, 1);
	TestEqual(TEXT("coordinator observes one target compile"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("failed recovery compile runs once"), Outcome.RecoveryCompileCount, 1);
	TestEqual(TEXT("coordinator observes one recovery compile"), Operations.RecoveryCompiles, 1);
	TestEqual(TEXT("compile failure remains the operation result"), Error.ErrorCode, FString(TEXT("COMPILE_FAILED")));
	TestEqual(TEXT("exact error-state restoration verifies"), Outcome.RollbackStatus, FString(TEXT("restored")));
	TestFalse(TEXT("exact error-state restoration does not block the asset"), Outcome.bBlocked);
	TestEqual(TEXT("BS_Error is restored exactly"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	TestEqual(TEXT("generated state digest is identical"),
		FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint), GeneratedBefore);
	TestEqual(TEXT("authoring graph is restored exactly"), CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("dirty baseline is restored"), Fixture.Package->IsDirty(), bDirtyBefore);
	TestEqual(TEXT("failed recovery does not save"), Operations.Saves, 0);
	Operations.End();
	Fixture.Cleanup();
	IFileManager::Get().Delete(*Filename, false, true, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireCompileOnceTest,
	"Cortex.Graph.Authoring.Migration.Retire.CompileOnceAfterReparent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireCompileOnceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created under its legacy parent"),
		Fixture.Build(TEXT("BP_RetireCompileOnce"), false, false, nullptr, false, true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	IFileManager::Get().Delete(*Filename, false, true, true);
	TestTrue(TEXT("legacy-parent Widget is saved before reparenting"), Fixture.SaveToDisk());
	Fixture.Blueprint->ParentClass = UCortexGraphRetireTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);
	UK2Node_CustomEvent* NativeNameCollision = Fixture.AddNativeNameCollision();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestEqual(TEXT("baseline compiler collision establishes BS_Error status"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	Fixture.Graph->RemoveNode(NativeNameCollision);
	TestTrue(TEXT("stale graph entries are created after baseline compilation"), Fixture.PopulateGraph(true));
	Fixture.Retained->EventReference.SetExternalMember(TEXT("OnRetainedEvent"), UCortexGraphRetireTargetWidget::StaticClass());
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("approved compile-once request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107402"),
			Request, Approved, Error, true));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("retirement compiles and verifies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("successful retirement target compile count is one"), Outcome.TargetCompileCount, 1);
	TestEqual(TEXT("one real target compile is observed"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("successful retirement does not recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("compile status is compiled"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("readback status is matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("successful retirement does not save"), Operations.Saves, 0);
	for (const FString& GuidText : Approved)
	{
		FGuid Guid;
		FGuid::Parse(GuidText, Guid);
		TestNull(TEXT("retired entry/body/producer is absent"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
	}
	TestNotNull(TEXT("retained override remains"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.Retained->NodeGuid));
	TestNotNull(TEXT("retained body remains"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.RetainedBody->NodeGuid));
	Operations.End();
	Fixture.Cleanup();
	IFileManager::Get().Delete(*Filename, false, true, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireStagedBytesTest,
	"Cortex.Graph.Authoring.Migration.Retire.StagedApplyPreservesDiskBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireStagedBytesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireStagedBytes"), false, false, nullptr, false, true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	IFileManager::Get().Delete(*Filename, false, true, true);
	TestTrue(TEXT("clean baseline package is saved"), Fixture.SaveToDisk());
	const TArray<uint8> DiskBefore = ReadBytes(Filename);
	Fixture.AddNativeNameCollision();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestEqual(TEXT("in-memory package starts invalid"), static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	Fixture.Blueprint->ParentClass = UCortexGraphRetireCollisionTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);
	FBPVariableDescription& CollisionVariable = Fixture.Blueprint->NewVariables.AddDefaulted_GetRef();
	CollisionVariable.VarName = FName(TEXT("NativeCollision"));
	CollisionVariable.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
	Fixture.Package->MarkPackageDirty();
	TestTrue(TEXT("stale graph entries are created after failed compilation"), Fixture.PopulateGraph(false));
	Fixture.Blueprint->Status = BS_Error;
	Fixture.Retained->EventReference.SetExternalMember(TEXT("OnRetainedEvent"), UCortexGraphRetireTargetWidget::StaticClass());
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("staged request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107403"),
			Request, Approved, Error, false, false));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("staged retirement applies and reads back: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("staged compile status is not_requested"), Outcome.CompileStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("staged readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestFalse(TEXT("staged operation does not claim save"), Outcome.bSaved);
	TestEqual(TEXT("staged operation does not compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("staged operation does not save"), Operations.Saves, 0);
	TestTrue(TEXT("staged operation leaves package dirty"), Fixture.Package->IsDirty());
	TestEqual(TEXT("staged mutation leaves the compiler status dirty"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Dirty));
	TestTrue(TEXT("cached compiler information does not claim a successful compile"),
		Outcome.CompileStatus != TEXT("compiled"));
	TestTrue(TEXT("on-disk bytes are unchanged"), SameBytes(DiskBefore, ReadBytes(Filename)));
	Operations.End();
	Fixture.Cleanup();
	IFileManager::Get().Delete(*Filename, false, true, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireSaveCleanStartTest,
	"Cortex.Graph.Authoring.Migration.Retire.SaveAfterVerifiedCleanStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireSaveCleanStartTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireSaveClean"), false, false, nullptr, false, true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	IFileManager::Get().Delete(*Filename, false, true, true);
	TestTrue(TEXT("legacy-parent baseline saves"), Fixture.SaveToDisk());
	Fixture.Blueprint->ParentClass = UCortexGraphRetireTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);
	UK2Node_CustomEvent* NativeNameCollision = Fixture.AddNativeNameCollision();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestEqual(TEXT("invalid reparented package is BS_Error before recovery"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	Fixture.Graph->RemoveNode(NativeNameCollision);
	TestTrue(TEXT("stale graph entries are created after baseline compilation"), Fixture.PopulateGraph(true));
	Fixture.Retained->EventReference.SetExternalMember(TEXT("OnRetainedEvent"), UCortexGraphRetireTargetWidget::StaticClass());
	TestTrue(TEXT("invalid reparented state is persisted as a clean starting package"), Fixture.SaveToDisk());
	TestFalse(TEXT("package begins clean"), Fixture.Package->IsDirty());
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("save request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107404"),
			Request, Approved, Error, true, true));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("verified retirement saves: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("post-save status is saved"), Outcome.SaveStatus, FString(TEXT("saved")));
	TestEqual(TEXT("post-save verification is verified"), Outcome.PostSaveStatus, FString(TEXT("verified")));
	TestTrue(TEXT("outcome reports saved"), Outcome.bSaved);
	TestEqual(TEXT("one target compile precedes save"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("one explicit package save occurs"), Operations.Saves, 1);
	TestFalse(TEXT("verified saved package is clean"), Fixture.Package->IsDirty());
	const FString PackageName = Fixture.Package->GetName();
	const FString ObjectName = Fixture.Blueprint->GetName();
	const FGuid GraphGuid = Fixture.Graph->GraphGuid;
	const FGuid RetainedGuid = Fixture.Retained->NodeGuid;
	const FGuid RetainedBodyGuid = Fixture.RetainedBody->NodeGuid;
	const FGuid ProducerGuid = Fixture.Producer->NodeGuid;
	const TArray<FString> RetiredGuidTexts = Approved;
	UPackage* const PackageBeforeReload = Fixture.Package;
	UBlueprint* const BlueprintBeforeReload = Fixture.Blueprint;
	Operations.End();

	TArray<UPackage*> PackagesToReload;
	PackagesToReload.Add(PackageBeforeReload);
	FText ReloadError;
	const bool bReloaded = UPackageTools::ReloadPackages(
		PackagesToReload, ReloadError, EReloadPackagesInteractionMode::AssumeNegative);
	TestTrue(FString::Printf(TEXT("saved retirement package reloads: %s"), *ReloadError.ToString()), bReloaded);
	UPackage* ReloadedPackage = FindPackage(nullptr, *PackageName);
	UWidgetBlueprint* Reloaded = ReloadedPackage
		? FindObject<UWidgetBlueprint>(ReloadedPackage, *ObjectName) : nullptr;
	TestNotNull(TEXT("saved Widget Blueprint resolves after reload"), Reloaded);
	if (Reloaded)
	{
		TestTrue(TEXT("reload replaced the in-memory Blueprint instance"), Reloaded != BlueprintBeforeReload);
		TArray<UEdGraph*> ReloadedGraphs;
		Reloaded->GetAllGraphs(ReloadedGraphs);
		UEdGraph* ReloadedGraph = nullptr;
		for (UEdGraph* Candidate : ReloadedGraphs)
		{
			if (Candidate && Candidate->GraphGuid == GraphGuid)
			{
				ReloadedGraph = Candidate;
				break;
			}
		}
		TestNotNull(TEXT("retained event graph resolves from disk"), ReloadedGraph);
		auto FindNodeInGraph = [](UEdGraph* Graph, const FGuid& Guid) -> UEdGraphNode*
		{
			if (!Graph) return nullptr;
			for (UEdGraphNode* Node : Graph->Nodes) if (Node && Node->NodeGuid == Guid) return Node;
			return nullptr;
		};
		UEdGraphNode* ReloadedRetained = FindNodeInGraph(ReloadedGraph, RetainedGuid);
		UEdGraphNode* ReloadedRetainedBody = FindNodeInGraph(ReloadedGraph, RetainedBodyGuid);
		UEdGraphNode* ReloadedProducer = FindNodeInGraph(ReloadedGraph, ProducerGuid);
		TestNotNull(TEXT("retained event survives in the reloaded graph"), ReloadedRetained);
		TestNotNull(TEXT("retained body survives in the reloaded graph"), ReloadedRetainedBody);
		TestNotNull(TEXT("shared producer survives in the reloaded graph"), ReloadedProducer);
		for (const FString& GuidText : RetiredGuidTexts)
		{
			FGuid Guid;
			FGuid::Parse(GuidText, Guid);
			TestNull(TEXT("retired GUID is absent from the reloaded asset"),
				FCortexGraphMigrationOps::FindNodeByGuid(Reloaded, Guid));
		}
		if (ReloadedRetained && ReloadedRetainedBody && ReloadedProducer)
		{
			UEdGraphPin* RetainedThen = ReloadedRetained->FindPin(TEXT("then"));
			UEdGraphPin* RetainedBodyExecute = ReloadedRetainedBody->FindPin(TEXT("execute"));
			UEdGraphPin* ProducerOutput = ReloadedProducer->FindPin(TEXT("ReturnValue"));
			UEdGraphPin* RetainedBodyInput = ReloadedRetainedBody->FindPin(TEXT("InString"));
			TestNotNull(TEXT("reloaded retained entry has its exec pin"), RetainedThen);
			TestNotNull(TEXT("reloaded retained body has its exec input"), RetainedBodyExecute);
			TestNotNull(TEXT("reloaded producer has its output pin"), ProducerOutput);
			TestNotNull(TEXT("reloaded retained body has its data input"), RetainedBodyInput);
			if (RetainedThen && RetainedBodyExecute)
			{
				TestTrue(TEXT("retained entry-to-body link survives the disk reload"),
					RetainedThen->LinkedTo.Contains(RetainedBodyExecute));
			}
			if (ProducerOutput && RetainedBodyInput)
			{
				TestTrue(TEXT("retained producer-to-body link survives the disk reload"),
					ProducerOutput->LinkedTo.Contains(RetainedBodyInput));
			}
		}
	}

	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphMigrationRetireSaveReloadCleanup")));
	}
	UPackage* PackageToCleanup = ReloadedPackage ? ReloadedPackage : PackageBeforeReload;
	if (PackageToCleanup)
	{
		PackageToCleanup->ClearFlags(RF_Standalone);
		PackageToCleanup->MarkAsGarbage();
		ResetLoaders(PackageToCleanup);
	}
	Fixture.Package = nullptr;
	Fixture.Blueprint = nullptr;
	FlushAsyncLoading();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("reloaded retirement fixture file is removed"),
		IFileManager::Get().Delete(*Filename, false, true, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireSaveDirtyStartTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesDirtyStartSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireSaveDirtyStartTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireSaveDirty"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	IFileManager::Get().Delete(*Filename, false, true, true);
	TestTrue(TEXT("baseline package is saved"), Fixture.SaveToDisk());
	const TArray<uint8> DiskBefore = ReadBytes(Filename);
	Fixture.Retained->NodeComment = TEXT("unrelated retained edit");
	FBlueprintEditorUtils::MarkBlueprintAsModified(Fixture.Blueprint);
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("dirty-start save request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107405"),
			Request, Approved, Error, true, true));
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("dirty-start save is refused"), FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("dirty-start refusal reports DIRTY_EDITOR_STATE"), Error.ErrorCode, FString(TEXT("DIRTY_EDITOR_STATE")));
	TestEqual(TEXT("preflight refusal has no target compile"), Outcome.TargetCompileCount, 0);
	TestEqual(TEXT("dirty start has no recovery compile"), Outcome.RecoveryCompileCount, 0);
	TestEqual(TEXT("dirty edit remains in memory"), CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestTrue(TEXT("dirty baseline remains dirty"), Fixture.Package->IsDirty());
	TestTrue(TEXT("disk bytes remain unchanged"), SameBytes(DiskBefore, ReadBytes(Filename)));
	Fixture.Cleanup();
	IFileManager::Get().Delete(*Filename, false, true, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAbsentSourceReplayTest,
	"Cortex.Graph.Authoring.Migration.Retire.AbsentSourceReplayIsUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAbsentSourceReplayTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture is created"), Fixture.Build(TEXT("BP_RetireReplay"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	TSharedPtr<FJsonObject> InitialRequest;
	TArray<FString> Approved;
	FCortexCommandResult Error;
	TestTrue(TEXT("initial reviewed request is prepared"),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107501"),
			InitialRequest, Approved, Error));
	FCortexGraphPatchOutcome InitialOutcome;
	TestTrue(FString::Printf(TEXT("initial retirement applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, InitialRequest, InitialOutcome, Error));
	TestEqual(TEXT("initial retirement applies"), InitialOutcome.ApplyStatus, FString(TEXT("applied")));

	const FString GraphAfterRetirement = CaptureNativeGraph(Fixture.Graph);
	const TSharedPtr<FJsonObject> FingerprintAfterRetirement =
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint);
	const FString FingerprintHashAfterRetirement =
		FingerprintAfterRetirement->GetStringField(TEXT("graph_authoring_hash"));
	const bool bDirtyAfterRetirement = Fixture.Package->IsDirty();
	const int32 QueueBeforeReplay = GEditor->Trans->GetQueueLength();
	const int32 UndoBeforeReplay = GEditor->Trans->GetUndoCount();
	const int32 NodeCountBeforeReplay = Fixture.Graph->Nodes.Num();
	TSharedPtr<FJsonObject> ReplayRequest = MakeShared<FJsonObject>();
	ReplayRequest->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	ReplayRequest->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-000000107501"));
	ReplayRequest->SetObjectField(TEXT("expected_fingerprint"), FingerprintAfterRetirement);
	ReplayRequest->SetArrayField(TEXT("nodes"), {});
	ReplayRequest->SetArrayField(TEXT("connections"), {});
	ReplayRequest->SetArrayField(TEXT("pin_updates"), {});
	ReplayRequest->SetBoolField(TEXT("dry_run"), true);
	ReplayRequest->SetBoolField(TEXT("compile"), false);
	ReplayRequest->SetBoolField(TEXT("save"), false);
	ReplayRequest->SetBoolField(TEXT("allow_noop"), false);
	ReplayRequest->SetObjectField(TEXT("migration"), Fixture.Migration(
		{ Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() }, true, Approved));

	FCortexGraphPreparedPatch ReplayPreview;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("fresh approved replay preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, ReplayRequest, ReplayPreview, Error));
	TestTrue(TEXT("replay preview reuses the retirement plan"), ReplayPreview.RetirementPlan.IsValid());
	TestFalse(TEXT("replay preview reports no graph change"), ReplayPreview.bChanged);
	TestTrue(TEXT("replay preview records absent-source provenance"), ReplayPreview.bReplayedWithAbsentSource);
	TestTrue(TEXT("replay preview carries a fresh validation hash"),
		!ReplayPreview.ValidationHash.IsEmpty()
			&& ReplayPreview.ValidationHash != InitialRequest->GetStringField(TEXT("expected_validation_hash")));
	if (ReplayPreview.RetirementPlan.IsValid())
	{
		const TSharedPtr<FJsonObject> Inventory =
			FCortexGraphMigrationOps::MakeRetirementInventory(ReplayPreview.RetirementPlan);
		TestTrue(TEXT("replay is marked reused"), Inventory.IsValid() && Inventory->GetBoolField(TEXT("reused")));
		TestTrue(TEXT("replay inventory is complete"), Inventory.IsValid() && Inventory->GetBoolField(TEXT("complete")));
		TestFalse(TEXT("replay inventory is not awaiting approval"),
			Inventory.IsValid() && Inventory->GetBoolField(TEXT("awaiting_approval")));
	}
	ReplayRequest->SetBoolField(TEXT("dry_run"), false);
	ReplayRequest->SetStringField(TEXT("expected_validation_hash"), ReplayPreview.ValidationHash);

	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome ReplayOutcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("freshly reviewed replay applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, ReplayRequest, ReplayOutcome, Error));
	Operations.End();
	TestEqual(TEXT("replay reports unchanged"), ReplayOutcome.ApplyStatus, FString(TEXT("unchanged")));
	TestFalse(TEXT("replay reports no graph change"), ReplayOutcome.bChanged);
	TestTrue(TEXT("replay outcome reports reused retirement inventory"),
		ReplayOutcome.RetirementInventory.IsValid()
			&& ReplayOutcome.RetirementInventory->GetBoolField(TEXT("reused")));
	TestTrue(TEXT("replay records absent-source provenance"), ReplayOutcome.bReplayedWithAbsentSource);
	TestEqual(TEXT("replay creates no transaction"), GEditor->Trans->GetQueueLength(), QueueBeforeReplay);
	TestEqual(TEXT("replay creates no undo record"), GEditor->Trans->GetUndoCount(), UndoBeforeReplay);
	TestEqual(TEXT("replay requests no target compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("replay requests no recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("replay does not save"), Operations.Saves, 0);
	TestFalse(TEXT("replay does not claim save"), ReplayOutcome.bSaved);
	TestEqual(TEXT("replay preserves graph state"), CaptureNativeGraph(Fixture.Graph), GraphAfterRetirement);
	TestEqual(TEXT("replay preserves graph node count"), Fixture.Graph->Nodes.Num(), NodeCountBeforeReplay);
	TestEqual(TEXT("replay preserves fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
		->GetStringField(TEXT("graph_authoring_hash")), FingerprintHashAfterRetirement);
	TestEqual(TEXT("replay preserves package dirty state"), Fixture.Package->IsDirty(), bDirtyAfterRetirement);
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetirePartialReplayRefusalTest,
	"Cortex.Graph.Authoring.Migration.Retire.PartialReplayNamesAbsentAndPresentIdentities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetirePartialReplayRefusalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture is created"), Fixture.Build(TEXT("BP_RetirePartialReplay"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	TSharedPtr<FJsonObject> InitialRequest;
	TArray<FString> Approved;
	FCortexCommandResult Error;
	TestTrue(TEXT("initial reviewed request is prepared"),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107502"),
			InitialRequest, Approved, Error));
	FCortexGraphPatchOutcome Outcome;
	TestTrue(TEXT("initial retirement applies"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, InitialRequest, Outcome, Error));

	UK2Node_CustomEvent* Reintroduced = NewObject<UK2Node_CustomEvent>(Fixture.Graph);
	Reintroduced->NodeGuid = FGuid(Approved[0]);
	Fixture.Graph->AddNode(Reintroduced, true, false);
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	Error = FCortexCommandResult();
	TestFalse(TEXT("partial postcondition is refused"),
		Plan(Fixture, { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() },
			PlanValue, bReused, Error, true, Approved));
	TestEqual(TEXT("partial replay reports INVALID_OPERATION"), Error.ErrorCode, FString(TEXT("INVALID_OPERATION")));
	TArray<FString> ExpectedAbsent = Approved;
	ExpectedAbsent.RemoveAt(0);
	ExpectedAbsent.Sort();
	const FString ExpectedPartialDiagnostic = FString::Printf(
		TEXT("retirement replay is partial: %d approved identities are absent [%s] and 1 are present [%s]"),
		Approved.Num() - 1, *FString::Join(ExpectedAbsent, TEXT(", ")), *Approved[0]);
	TestEqual(TEXT("partial replay reports exact absent and present counts and identities"),
		Error.ErrorMessage, ExpectedPartialDiagnostic);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireOwnershipConflictTest,
	"Cortex.Graph.Authoring.Migration.Retire.AbsentSourceOwnershipConflictIsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireOwnershipConflictTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture is created"), Fixture.Build(TEXT("BP_RetireOwnershipConflict"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	TSharedPtr<FJsonObject> InitialRequest;
	TArray<FString> Approved;
	FCortexCommandResult Error;
	TestTrue(TEXT("initial reviewed request is prepared"),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107503"),
			InitialRequest, Approved, Error));
	FCortexGraphPatchOutcome Outcome;
	TestTrue(TEXT("initial retirement applies"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, InitialRequest, Outcome, Error));

	UEdGraph* OtherGraph = NewObject<UEdGraph>(Fixture.Blueprint);
	OtherGraph->GraphGuid = FGuid::NewGuid();
	Fixture.Blueprint->FunctionGraphs.Add(OtherGraph);
	UK2Node_CustomEvent* Reowned = NewObject<UK2Node_CustomEvent>(OtherGraph);
	Reowned->NodeGuid = FGuid(Approved[0]);
	OtherGraph->AddNode(Reowned, true, false);
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	Error = FCortexCommandResult();
	TestFalse(TEXT("approved absent GUID owned by another graph is refused"),
		Plan(Fixture, { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() },
			PlanValue, bReused, Error, true, Approved));
	TestEqual(TEXT("ownership conflict reports INVALID_OPERATION"), Error.ErrorCode, FString(TEXT("INVALID_OPERATION")));
	TestTrue(TEXT("ownership conflict names the conflicting GUID and graph"),
		Error.ErrorMessage.Contains(Approved[0]) && Error.ErrorMessage.Contains(OtherGraph->GraphGuid.ToString()));
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireDiagnosticsTruncationTest,
	"Cortex.Graph.Authoring.Migration.Retire.DiagnosticsTruncationIsTruthful",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireDiagnosticsTruncationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;

	// Case A: Long cached diagnostic (> 512 chars)
	{
		FFixture Fixture;
		TestTrue(TEXT("Case A fixture created"), Fixture.Build(TEXT("BP_RetireDiagLong")));
		if (Fixture.Blueprint)
		{
			Fixture.Alpha->bHasCompilerMessage = true;
			Fixture.Alpha->ErrorMsg = FString::ChrN(700, TEXT('x'));

			FCortexGraphMigrationRetirePlan PlanValue;
			bool bReused = false;
			FCortexCommandResult Error;
			TestTrue(TEXT("preview succeeds with long diagnostic"),
				Plan(Fixture, { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() }, PlanValue, bReused, Error));
			TestEqual(TEXT("exactly one cached diagnostic collected"), PlanValue.PreexistingDiagnostics.Num(), 1);
			if (PlanValue.PreexistingDiagnostics.Num() == 1)
			{
				TestTrue(TEXT("returned string respects 512 character bound"), PlanValue.PreexistingDiagnostics[0].Len() <= 512);
			}
			TestTrue(TEXT("preexisting_diagnostics_truncated is true for character truncation"),
				PlanValue.bPreexistingDiagnosticsTruncated);

			const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanValue.ToJson());
			TestNotNull(TEXT("inventory is valid"), Inventory.Get());
			if (Inventory.IsValid())
			{
				TestTrue(TEXT("inventory reports preexisting_diagnostics_truncated true"),
					Inventory->GetBoolField(TEXT("preexisting_diagnostics_truncated")));
				TestEqual(TEXT("inventory names cached_node_messages source"),
					Inventory->GetStringField(TEXT("preexisting_diagnostics_source")), FString(TEXT("cached_node_messages")));
			}
			Fixture.Cleanup();
		}
	}

	// Case B: Count truncation (> 16 messages)
	{
		FFixture Fixture;
		TestTrue(TEXT("Case B fixture created"), Fixture.Build(TEXT("BP_RetireDiagCount")));
		if (Fixture.Blueprint)
		{
			for (int32 Index = 0; Index < 20; ++Index)
			{
				UK2Node_CallFunction* Dummy = Fixture.AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
				Dummy->bHasCompilerMessage = true;
				Dummy->ErrorMsg = FString::Printf(TEXT("error message %d"), Index);
			}

			FCortexGraphMigrationRetirePlan PlanValue;
			bool bReused = false;
			FCortexCommandResult Error;
			TestTrue(TEXT("preview succeeds with many diagnostics"),
				Plan(Fixture, { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() }, PlanValue, bReused, Error));
			TestTrue(TEXT("diagnostics count bounded by 16"), PlanValue.PreexistingDiagnostics.Num() <= 16);
			TestTrue(TEXT("omission marker is present"),
				PlanValue.PreexistingDiagnostics.Contains(TEXT("additional compiler diagnostics omitted")));
			TestTrue(TEXT("preexisting_diagnostics_truncated is true for count truncation"),
				PlanValue.bPreexistingDiagnosticsTruncated);

			const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanValue.ToJson());
			TestNotNull(TEXT("inventory is valid"), Inventory.Get());
			if (Inventory.IsValid())
			{
				TestTrue(TEXT("inventory reports preexisting_diagnostics_truncated true for count truncation"),
					Inventory->GetBoolField(TEXT("preexisting_diagnostics_truncated")));
			}
			Fixture.Cleanup();
		}
	}

	// Case C: No truncation (single short message)
	{
		FFixture Fixture;
		TestTrue(TEXT("Case C fixture created"), Fixture.Build(TEXT("BP_RetireDiagShort")));
		if (Fixture.Blueprint)
		{
			Fixture.Alpha->bHasCompilerMessage = true;
			Fixture.Alpha->ErrorMsg = TEXT("short compiler warning message");

			FCortexGraphMigrationRetirePlan PlanValue;
			bool bReused = false;
			FCortexCommandResult Error;
			TestTrue(TEXT("preview succeeds with short diagnostic"),
				Plan(Fixture, { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() }, PlanValue, bReused, Error));
			TestEqual(TEXT("exactly one cached diagnostic collected"), PlanValue.PreexistingDiagnostics.Num(), 1);
			TestFalse(TEXT("preexisting_diagnostics_truncated is false when no truncation occurs"),
				PlanValue.bPreexistingDiagnosticsTruncated);

			const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanValue.ToJson());
			TestNotNull(TEXT("inventory is valid"), Inventory.Get());
			if (Inventory.IsValid())
			{
				TestFalse(TEXT("inventory reports preexisting_diagnostics_truncated false when no truncation"),
					Inventory->GetBoolField(TEXT("preexisting_diagnostics_truncated")));
			}
			Fixture.Cleanup();
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireConformEngineTransitionTest,
	"Cortex.Graph.Authoring.Migration.Retire.PostReparentCompileConformsStaleOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireConformEngineTransitionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;

	FFixture Fixture;
	TestTrue(TEXT("Build legacy parent widget fixture"),
		Fixture.Build(TEXT("BP_RetireEngineTransition"), false, false, UCortexGraphRetireLegacyWidget::StaticClass()));
	if (!Fixture.Blueprint) return false;

	// 1. Before compile / reparent: genuine eligible override event
	TestNotNull(TEXT("Alpha exists before reparent"), Fixture.Alpha);
	TestTrue(TEXT("Alpha is UK2Node_Event before reparent"), Fixture.Alpha != nullptr && !Fixture.Alpha->IsA<UK2Node_CustomEvent>());
	if (Fixture.Alpha)
	{
		TestTrue(TEXT("Alpha is an override function before reparent"), Fixture.Alpha->bOverrideFunction);
		TestEqual(TEXT("Alpha member name is OnLegacyAlpha"),
			Fixture.Alpha->EventReference.GetMemberName(), FName(TEXT("OnLegacyAlpha")));
	}
	const FGuid OriginalAlphaGuid = Fixture.AlphaGuid;

	// 2. Reparent to target widget (where OnLegacyAlpha does not exist)
	Fixture.Blueprint->ParentClass = UCortexGraphRetireCollisionTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);

	// 3. Add an independent deterministic defect causing compile failure
	Fixture.AddNativeNameCollision();

	// 4. Compile: Blueprint enters real BS_Error
	// In UE 5.8, ConformImplementedEvents emits a warning for each stale override conformed to custom event
	AddExpectedError(TEXT("replaced as a Custom Event"), EAutomationExpectedErrorFlags::Contains, 2);
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	TestEqual(TEXT("Blueprint entered real BS_Error status"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));

	// 5. In UE 5.8, FBlueprintEditorUtils::ConformImplementedEvents destroys stale override UK2Node_Event
	// and substitutes a UK2Node_CustomEvent that preserves the original NodeGuid
	UK2Node_CustomEvent* SubstituteCustomEvent = nullptr;
	for (UEdGraphNode* Node : Fixture.Graph->Nodes)
	{
		if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
		{
			if (CustomEvent->CustomFunctionName.ToString().Contains(TEXT("OnLegacyAlpha")))
			{
				SubstituteCustomEvent = CustomEvent;
				break;
			}
		}
	}
	TestNotNull(TEXT("stale override node was conformed into a UK2Node_CustomEvent by UE 5.8 compiler"),
		SubstituteCustomEvent);
	if (SubstituteCustomEvent)
	{
		TestEqual(TEXT("substitute custom event inherits the original node GUID"),
			SubstituteCustomEvent->NodeGuid, OriginalAlphaGuid);
		TestTrue(TEXT("substitute node is a UK2Node_CustomEvent"),
			SubstituteCustomEvent->IsA<UK2Node_CustomEvent>());
		TestFalse(TEXT("substitute node no longer has bOverrideFunction"),
			SubstituteCustomEvent->bOverrideFunction);
	}

	// 6. Prove the conformed node is refused by retire_entries because it is now a custom event:
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestFalse(TEXT("retire_entries refuses conformed custom event"),
		Plan(Fixture, { OriginalAlphaGuid.ToString() }, PlanValue, bReused, Error));
	TestEqual(TEXT("refusal error code is INVALID_OPERATION"), Error.ErrorCode, FString(TEXT("INVALID_OPERATION")));
	TestTrue(TEXT("error message explains node is not a supported unbound override event"),
		Error.ErrorMessage.Contains(TEXT("not a supported unbound override event")));

	Fixture.Cleanup();
	return true;
}

#endif

