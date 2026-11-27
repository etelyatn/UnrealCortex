#include "Misc/AutomationTest.h"

#include "CortexAdoptTestHelpers.h"
#include "CortexBPEmbeddedTestTypes.h"
#include "CortexBPReparentTestSupport.h"
#include "CortexCommandRouter.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPReparentOps.h"
#include "UObject/Package.h"

namespace
{
	/** Two custom events with one name: the compile after the parent change reports an error. */
	void AddDuplicateEvents(UBlueprint* Target)
	{
		UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Target);
		for (int32 Index = 0; Index < 2; ++Index)
		{
			UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
			Event->CreateNewGuid();
			Event->CustomFunctionName = FName(TEXT("ReparentRecoveryDuplicateEvent"));
			EventGraph->AddNode(Event, false, false);
			Event->AllocateDefaultPins();
		}
	}

	struct FReparentRecoveryHookScope
	{
		explicit FReparentRecoveryHookScope(TFunction<void(UBlueprint*)> Hook)
		{
			FCortexBPReparentOps::SetPostApplyHookForTest(MoveTemp(Hook));
		}
		~FReparentRecoveryHookScope()
		{
			FCortexBPReparentOps::ClearPostApplyHookForTest();
		}
	};

	UK2Node_CallFunction* AddCallThrough(UBlueprint* Blueprint, UK2Node_VariableGet* Get, UClass* Owner, const TCHAR* FunctionName)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
		UK2Node_CallFunction* Call = Creator.CreateNode();
		Call->FunctionReference.SetExternalMember(FName(FunctionName), Owner);
		Creator.Finalize();
		UEdGraphPin* SelfPin = Call->FindPin(UEdGraphSchema_K2::PN_Self);
		if (SelfPin == nullptr || !GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Get->GetValuePin(), SelfPin))
		{
			return nullptr;
		}
		return Call;
	}

	TSharedPtr<FJsonObject> FindCallSite(const TSharedPtr<FJsonObject>& Data, const FString& NodeId)
	{
		const TArray<TSharedPtr<FJsonValue>>* Sites = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(TEXT("type_changing_call_sites"), Sites) && Sites != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Sites)
			{
				const TSharedPtr<FJsonObject>* Site = nullptr;
				FString Id;
				if (Value->TryGetObject(Site) && Site != nullptr && (*Site)->TryGetStringField(TEXT("node_id"), Id) && Id == NodeId)
				{
					return *Site;
				}
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReparentRestoresCdoAfterFailureTest,
	"Cortex.Blueprint.Reparent.RestoresCdoAfterFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReparentRestoresCdoAfterFailureTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("GuardedCdoRestore"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AActor* Cdo = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
	const float LifeSpanBefore = Cdo->InitialLifeSpan;

	// The apply step changes a CDO value (an operation's side effect the undo and the generic journal do not cover), then the readback fails.
	FCortexBPGuardedRun::FSpec Spec;
	Spec.OpName = TEXT("cdo_journal_test");
	Spec.Blueprint = Blueprint;
	Spec.bCompile = false;
	Spec.bJournalCdo = true;
	Spec.Apply = [](UBlueprint* Target, FString&)
	{
		Cast<AActor>(Target->GeneratedClass->GetDefaultObject())->InitialLifeSpan = 9.f;
		return true;
	};
	Spec.Verify = [](UBlueprint*, bool, FString& OutError) { OutError = TEXT("forced readback mismatch"); return false; };

	const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
	TestFalse(TEXT("The run fails"), Result.bSuccess);
	if (TestTrue(TEXT("Failure carries details"), Result.ErrorDetails.IsValid()))
	{
		TestTrue(TEXT("rollback_cdo_restored"), Result.ErrorDetails->GetBoolField(TEXT("rollback_cdo_restored")));
		TestTrue(TEXT("The snapshot, which includes the CDO, matches again"), Result.ErrorDetails->GetBoolField(TEXT("rollback_snapshot_matches")));
		TestEqual(TEXT("Rolled back"), Result.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
	}
	TestEqual(TEXT("The CDO value is back"), Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject())->InitialLifeSpan, LifeSpanBefore);

	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReparentRestoresCdoChangedByFailedCompileTest,
	"Cortex.Blueprint.Reparent.RestoresCdoChangedByFailedCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReparentRestoresCdoChangedByFailedCompileTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("ReparentCdoChanged"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	// A non-zero pre-call value that the new native parent's constructor default (7) and the injected change (9) both differ from.
	Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject())->InitialLifeSpan = 3.f;

	FCortexCommandResult Result;
	{
		FReparentRecoveryHookScope Hook([](UBlueprint* BlueprintToBreak)
		{
			// The failed reparent run leaves the CDO changed (the compile's regeneration is simulated by writing the CDO), and the compile fails.
			Cast<AActor>(BlueprintToBreak->GeneratedClass->GetDefaultObject())->InitialLifeSpan = 9.f;
			AddDuplicateEvents(BlueprintToBreak);
		});
		Result = FCortexBPReparentOps::Execute(ReparentParams(Blueprint, ACortexBPAdoptParent::StaticClass()->GetPathName()));
	}

	TestFalse(TEXT("The forced compile failure fails"), Result.bSuccess);
	TestEqual(TEXT("Compile failure code"), Result.ErrorCode, CortexErrorCodes::CompileFailed);
	if (TestTrue(TEXT("Failure carries details"), Result.ErrorDetails.IsValid()))
	{
		bool bCdoRestored = false;
		TestTrue(TEXT("rollback_cdo_restored is reported"), Result.ErrorDetails->TryGetBoolField(TEXT("rollback_cdo_restored"), bCdoRestored));
		TestTrue(TEXT("The CDO journal restored every value"), bCdoRestored);
		FString RollbackStatus;
		Result.ErrorDetails->TryGetStringField(TEXT("rollback_status"), RollbackStatus);
		TestEqual(TEXT("Rolled back and verified"), RollbackStatus, FString(TEXT("restored")));
	}
	TestTrue(TEXT("Parent restored"), Blueprint->ParentClass == AActor::StaticClass());
	TestEqual(TEXT("The CDO value is the pre-call value again"), Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject())->InitialLifeSpan, 3.f);

	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReparentListsReloadPackagesAfterFailureTest,
	"Cortex.Blueprint.Reparent.ListsReloadPackagesAfterFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReparentListsReloadPackagesAfterFailureTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;

	UBlueprint* Target = MakeBlueprint(TEXT("ReloadTarget"), AActor::StaticClass());
	UBlueprint* Touched = MakeBlueprint(TEXT("ReloadTouched"), AActor::StaticClass());
	UBlueprint* Untouched = MakeBlueprint(TEXT("ReloadUntouched"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Target"), Target) || !TestNotNull(TEXT("Touched"), Touched) || !TestNotNull(TEXT("Untouched"), Untouched))
	{
		return false;
	}
	// Another loaded package that was dirty before the call and is never touched by it must not be listed.
	Touched->GetOutermost()->ClearDirtyFlag();
	Untouched->MarkPackageDirty();

	const FString NewParent = ACortexBPAdoptParent::StaticClass()->GetPathName();
	FCortexCommandResult Result;
	{
		// The failed compile dirties a dependent's package: simulated by dirtying one while the run is inside its apply step.
		FReparentRecoveryHookScope Hook([Touched](UBlueprint* BlueprintToBreak)
		{
			Touched->MarkPackageDirty();
			AddDuplicateEvents(BlueprintToBreak);
		});
		Result = FCortexBPReparentOps::Execute(ReparentParams(Target, NewParent));
	}

	TestFalse(TEXT("The forced compile failure fails"), Result.bSuccess);
	if (TestTrue(TEXT("Failure carries details"), Result.ErrorDetails.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>* Reload = nullptr;
		if (TestTrue(TEXT("reload_packages is reported"), Result.ErrorDetails->TryGetArrayField(TEXT("reload_packages"), Reload) && Reload != nullptr))
		{
			const TSharedPtr<FJsonObject>* TouchedEntry = nullptr;
			for (const TSharedPtr<FJsonValue>& Value : *Reload)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				FString PackageName;
				if (!Value->TryGetObject(Entry) || Entry == nullptr || !(*Entry)->TryGetStringField(TEXT("package"), PackageName))
				{
					AddError(TEXT("reload_packages holds an entry without a package"));
					continue;
				}
				TestNotEqual(TEXT("The target's own package is never listed"), PackageName, Target->GetOutermost()->GetName());
				TestNotEqual(TEXT("A package that was dirty before and untouched is not listed"), PackageName, Untouched->GetOutermost()->GetName());
				if (PackageName == Touched->GetOutermost()->GetName())
				{
					TouchedEntry = Entry;
				}
			}
			if (TestNotNull(TEXT("The package the run dirtied is listed"), TouchedEntry))
			{
				bool bWasDirty = true;
				TestTrue(TEXT("was_dirty_before reported"), (*TouchedEntry)->TryGetBoolField(TEXT("was_dirty_before"), bWasDirty));
				TestFalse(TEXT("was_dirty_before is false"), bWasDirty);
				TestTrue(TEXT("file field present"), (*TouchedEntry)->HasField(TEXT("file")));
			}
		}
		FString Instruction;
		TestTrue(TEXT("reload_instruction explains what to do"),
			Result.ErrorDetails->TryGetStringField(TEXT("reload_instruction"), Instruction) && Instruction.Contains(TEXT("Reload")));
		FString RollbackStatus;
		Result.ErrorDetails->TryGetStringField(TEXT("rollback_status"), RollbackStatus);
		TestEqual(TEXT("The target itself is rolled back"), RollbackStatus, FString(TEXT("restored")));
	}
	TestTrue(TEXT("Parent restored"), Target->ParentClass == AActor::StaticClass());

	Discard(Target);
	Discard(Touched);
	Discard(Untouched);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReparentTypeChangingCallSitesTest,
	"Cortex.Blueprint.Reparent.TypeChangingCallSites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReparentTypeChangingCallSitesTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("CallSites"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	FEdGraphPinType MeshComponentPin = MakePin(UEdGraphSchema_K2::PC_Object);
	MeshComponentPin.PinSubCategoryObject = UStaticMeshComponent::StaticClass();
	AddVariable(Blueprint, TEXT("Target"), MeshComponentPin);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UK2Node_VariableGet* GetForMesh = AddVariableGet(Blueprint, TEXT("Target"));
	UK2Node_VariableGet* GetForVisibility = AddVariableGet(Blueprint, TEXT("Target"));
	UK2Node_VariableGet* GetPassedAlong = AddVariableGet(Blueprint, TEXT("Target"));
	if (!TestNotNull(TEXT("Get nodes"), GetForMesh) || !TestNotNull(TEXT("Get nodes"), GetForVisibility) || !TestNotNull(TEXT("Get nodes"), GetPassedAlong))
	{
		Discard(Blueprint);
		return false;
	}
	// SetStaticMesh exists only on the derived component type; SetVisibility is declared by USceneComponent, a base of the new type.
	UK2Node_CallFunction* SetMesh = AddCallThrough(Blueprint, GetForMesh, UStaticMeshComponent::StaticClass(), TEXT("SetStaticMesh"));
	UK2Node_CallFunction* SetVisible = AddCallThrough(Blueprint, GetForVisibility, USceneComponent::StaticClass(), TEXT("SetVisibility"));
	if (!TestNotNull(TEXT("SetStaticMesh node"), SetMesh) || !TestNotNull(TEXT("SetVisibility node"), SetVisible))
	{
		Discard(Blueprint);
		return false;
	}
	const int32 NodesBefore = EventGraph->Nodes.Num();

	TSharedPtr<FJsonObject> DryRun = ReparentParams(Blueprint, ACortexBPBaseTypeParent::StaticClass()->GetPathName());
	DryRun->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult Plan = FCortexBPReparentOps::Execute(DryRun);
	if (TestTrue(TEXT("Dry run succeeds"), Plan.bSuccess) && Plan.Data.IsValid())
	{
		TestEqual(TEXT("Two nodes call through the retyped member; a get that is not called through is not a call site"),
			static_cast<int32>(Plan.Data->GetNumberField(TEXT("type_changing_call_site_count"))), 2);
		TestTrue(TEXT("A non-blocking warning is reported"), ReadStrings(Plan.Data, TEXT("warnings")).Num() == 1);
		TestEqual(TEXT("The warning does not block"), ReadStrings(Plan.Data, TEXT("blocking")).Num(), 0);

		const TSharedPtr<FJsonObject> MeshSite = FindCallSite(Plan.Data, SetMesh->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		if (TestNotNull(TEXT("SetStaticMesh call site"), MeshSite.Get()))
		{
			TestEqual(TEXT("member"), MeshSite->GetStringField(TEXT("member")), FString(TEXT("Target")));
			TestEqual(TEXT("old_type"), MeshSite->GetStringField(TEXT("old_type")), UStaticMeshComponent::StaticClass()->GetName());
			TestEqual(TEXT("new_type"), MeshSite->GetStringField(TEXT("new_type")), UPrimitiveComponent::StaticClass()->GetName());
			TestEqual(TEXT("graph"), MeshSite->GetStringField(TEXT("graph")), EventGraph->GetName());
			TestFalse(TEXT("SetStaticMesh no longer resolves on the base type"), MeshSite->GetBoolField(TEXT("resolves_on_new_type")));
		}
		const TSharedPtr<FJsonObject> VisibleSite = FindCallSite(Plan.Data, SetVisible->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		if (TestNotNull(TEXT("SetVisibility call site"), VisibleSite.Get()))
		{
			TestTrue(TEXT("SetVisibility still resolves on the base type"), VisibleSite->GetBoolField(TEXT("resolves_on_new_type")));
		}
	}
	TestEqual(TEXT("The dry run changed nothing"), EventGraph->Nodes.Num(), NodesBefore);
	TestTrue(TEXT("Parent unchanged"), Blueprint->ParentClass == AActor::StaticClass());

	// A Blueprint without such a member reports an empty list.
	UBlueprint* Plain = MakeBlueprint(TEXT("CallSitesPlain"), AActor::StaticClass());
	if (TestNotNull(TEXT("Plain Blueprint"), Plain))
	{
		TSharedPtr<FJsonObject> PlainDry = ReparentParams(Plain, ACortexBPBaseTypeParent::StaticClass()->GetPathName());
		PlainDry->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult PlainPlan = FCortexBPReparentOps::Execute(PlainDry);
		if (TestTrue(TEXT("Plain dry run succeeds"), PlainPlan.bSuccess) && PlainPlan.Data.IsValid())
		{
			TestEqual(TEXT("No call sites"), static_cast<int32>(PlainPlan.Data->GetNumberField(TEXT("type_changing_call_site_count"))), 0);
			TestFalse(TEXT("No warning"), PlainPlan.Data->HasField(TEXT("warnings")));
		}
		Discard(Plain);
	}

	// A discarded Blueprint stays reachable until the next collection: drop the nodes so other suites never resolve their members.
	for (UEdGraphNode* Node : TArray<UEdGraphNode*>{SetMesh, SetVisible, GetForMesh, GetForVisibility, GetPassedAlong})
	{
		EventGraph->RemoveNode(Node);
	}
	Discard(Blueprint);
	return true;
}
