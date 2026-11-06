#include "Misc/AutomationTest.h"

#include "CortexBPRenameMemberTestSupport.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "GameFramework/Actor.h"
#include "Operations/CortexBPRenameMemberOps.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameMemberVariableTest,
	"Cortex.Blueprint.RenameMember.Variable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameMemberVariableTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using namespace CortexRenameTest;
	UBlueprint* Blueprint = MakeBlueprint(TEXT("RenameVariable"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AddVariable(Blueprint, TEXT("bOpen"), BoolPin(), TEXT("true"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraph* Function = AddFunctionGraph(Blueprint, TEXT("Toggle"));
	if (!TestNotNull(TEXT("Function graph"), Function))
	{
		RetireBlueprint(Blueprint);
		return false;
	}
	UK2Node_VariableGet* EventGet = AddGet(EventGraph, TEXT("bOpen"));
	UK2Node_VariableGet* FunctionGet = AddGet(Function, TEXT("bOpen"));
	UK2Node_VariableSet* FunctionSet = AddSet(Function, TEXT("bOpen"));
	TestTrue(TEXT("Function nodes wired"), WireGetSet(Function, FunctionGet, FunctionSet));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("Fixture compiles"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

	const TArray<UK2Node_Variable*> Nodes = {EventGet, FunctionGet, FunctionSet};

	// dry_run: nothing changes, the nodes are listed.
	const FUnchanged Before(Blueprint);
	TSharedPtr<FJsonObject> DryParams = RenameParams(Blueprint, TEXT("variable"), TEXT("bOpen"), TEXT("bIsOpen"));
	DryParams->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult Dry = FCortexBPRenameMemberOps::Execute(DryParams);
	TSharedPtr<FJsonObject> Fingerprint;
	if (TestTrue(TEXT("dry_run succeeds"), Dry.bSuccess) && Dry.Data.IsValid())
	{
		TestFalse(TEXT("dry_run does not rename"), Dry.Data->GetBoolField(TEXT("renamed")));
		TestTrue(TEXT("dry_run can apply"), Dry.Data->GetBoolField(TEXT("can_apply")));
		TestEqual(TEXT("dry_run lists the three nodes"), CountAt(Dry.Data, TEXT("references"), TEXT("before")), 3);
		const TSharedPtr<FJsonObject> BeforeRefs = Dry.Data->GetObjectField(TEXT("references"))->GetObjectField(TEXT("before"));
		TSet<FString> Listed;
		for (const TSharedPtr<FJsonValue>& Value : BeforeRefs->GetArrayField(TEXT("references")))
		{
			Listed.Add(Value->AsObject()->GetStringField(TEXT("node_guid")));
		}
		for (const UK2Node_Variable* Node : Nodes)
		{
			TestTrue(TEXT("node listed by the dry run"), Listed.Contains(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
		}
		Fingerprint = Dry.Data->GetObjectField(TEXT("fingerprint_before"));
	}
	Before.Check(*this, Blueprint, TEXT("dry_run"));
	for (const UK2Node_Variable* Node : Nodes)
	{
		TestEqual(TEXT("dry_run leaves the node name"), Node->GetVarName(), FName(TEXT("bOpen")));
	}

	// apply with the fingerprint the dry run returned.
	TSharedPtr<FJsonObject> ApplyParams = RenameParams(Blueprint, TEXT("variable"), TEXT("bOpen"), TEXT("bIsOpen"));
	if (Fingerprint.IsValid())
	{
		ApplyParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	}
	const FCortexCommandResult Result = FCortexBPRenameMemberOps::Execute(ApplyParams);
	if (TestTrue(TEXT("rename succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestTrue(TEXT("renamed after readback"), Result.Data->GetBoolField(TEXT("renamed")));
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		const FString CompileStatus = Result.Data->GetStringField(TEXT("compile_status"));
		TestTrue(TEXT("compile clean"), CompileStatus == TEXT("success"));
		TestEqual(TEXT("references before"), CountAt(Result.Data, TEXT("references"), TEXT("before")), 3);
		TestEqual(TEXT("references after equal in count"), CountAt(Result.Data, TEXT("references"), TEXT("after")), 3);
	}
	else if (!Result.bSuccess)
	{
		AddError(FString::Printf(TEXT("rename failed: %s %s"), *Result.ErrorCode, *Result.ErrorMessage));
	}

	for (const UK2Node_Variable* Node : Nodes)
	{
		TestTrue(TEXT("ReferencesVariable(new)"), Node->ReferencesVariable(FName(TEXT("bIsOpen")), nullptr));
		TestFalse(TEXT("ReferencesVariable(old)"), Node->ReferencesVariable(FName(TEXT("bOpen")), nullptr));
	}
	TestTrue(TEXT("Variable renamed"), HasVariable(Blueprint, TEXT("bIsOpen")));
	TestFalse(TEXT("Old variable gone"), HasVariable(Blueprint, TEXT("bOpen")));
	UEdGraphPin* SetValuePin = FunctionSet->FindPin(FName(TEXT("bIsOpen")), EGPD_Input);
	TestTrue(TEXT("The set node's value pin follows the rename and stays connected"), SetValuePin != nullptr && SetValuePin->LinkedTo.Num() == 1);
	if (Blueprint->GeneratedClass != nullptr)
	{
		const FBoolProperty* Property = FindFProperty<FBoolProperty>(Blueprint->GeneratedClass, TEXT("bIsOpen"));
		if (TestNotNull(TEXT("Compiled property"), Property))
		{
			TestTrue(TEXT("Default value kept"), Property->GetPropertyValue_InContainer(Blueprint->GeneratedClass->GetDefaultObject()));
		}
	}

	ClearNodes(Blueprint);
	RetireBlueprint(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameMemberDispatcherTest,
	"Cortex.Blueprint.RenameMember.Dispatcher",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameMemberDispatcherTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using namespace CortexRenameTest;
	UBlueprint* Blueprint = MakeBlueprint(TEXT("RenameDispatcher"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AddDispatcher(Blueprint, TEXT("OnDone"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraph* const Signature = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, TEXT("OnDone"));
	if (!TestNotNull(TEXT("Signature graph"), Signature))
	{
		RetireBlueprint(Blueprint);
		return false;
	}

	FGraphNodeCreator<UK2Node_CallDelegate> CallCreator(*EventGraph);
	UK2Node_CallDelegate* Call = CallCreator.CreateNode();
	Call->DelegateReference.SetSelfMember(TEXT("OnDone"));
	CallCreator.Finalize();
	FGraphNodeCreator<UK2Node_AddDelegate> BindCreator(*EventGraph);
	UK2Node_AddDelegate* Bind = BindCreator.CreateNode();
	Bind->DelegateReference.SetSelfMember(TEXT("OnDone"));
	BindCreator.Finalize();
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// A variable rename of a dispatcher is refused with a pointer to the right kind.
	{
		const FUnchanged Unchanged(Blueprint);
		const FCortexCommandResult Wrong = FCortexBPRenameMemberOps::Execute(RenameParams(Blueprint, TEXT("variable"), TEXT("OnDone"), TEXT("OnFinished")));
		ExpectRefusal(*this, Wrong, CortexErrorCodes::InvalidField, TEXT("variable kind on a dispatcher"));
		Unchanged.Check(*this, Blueprint, TEXT("wrong kind"));
	}

	TSharedPtr<FJsonObject> DryParams = RenameParams(Blueprint, TEXT("dispatcher"), TEXT("OnDone"), TEXT("OnFinished"));
	DryParams->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult Dry = FCortexBPRenameMemberOps::Execute(DryParams);
	if (TestTrue(TEXT("dry_run succeeds"), Dry.bSuccess))
	{
		TestEqual(TEXT("dry_run lists the call and bind nodes"), CountAt(Dry.Data, TEXT("references"), TEXT("before")), 2);
	}

	const FCortexCommandResult Result = FCortexBPRenameMemberOps::Execute(RenameParams(Blueprint, TEXT("dispatcher"), TEXT("OnDone"), TEXT("OnFinished")));
	if (TestTrue(TEXT("dispatcher rename succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestTrue(TEXT("renamed"), Result.Data->GetBoolField(TEXT("renamed")));
		TestEqual(TEXT("compile clean"), Result.Data->GetStringField(TEXT("compile_status")), FString(TEXT("success")));
		TestEqual(TEXT("after count"), CountAt(Result.Data, TEXT("references"), TEXT("after")), 2);
	}
	else if (!Result.bSuccess)
	{
		AddError(FString::Printf(TEXT("dispatcher rename failed: %s %s"), *Result.ErrorCode, *Result.ErrorMessage));
	}

	TestTrue(TEXT("Signature graph kept (same object)"), FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, TEXT("OnFinished")) == Signature);
	TestNull(TEXT("Old signature name gone"), FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, TEXT("OnDone")));
	TestEqual(TEXT("One signature graph"), Blueprint->DelegateSignatureGraphs.Num(), 1);
	TestTrue(TEXT("Signature entry node kept"), EntryOf(Signature) != nullptr);
	TestEqual(TEXT("Call node follows"), Call->DelegateReference.GetMemberName(), FName(TEXT("OnFinished")));
	TestTrue(TEXT("Call node stays self context"), Call->DelegateReference.IsSelfContext());
	TestEqual(TEXT("Bind node follows"), Bind->DelegateReference.GetMemberName(), FName(TEXT("OnFinished")));
	TestTrue(TEXT("Variable renamed"), HasVariable(Blueprint, TEXT("OnFinished")));
	TestFalse(TEXT("Old variable gone"), HasVariable(Blueprint, TEXT("OnDone")));

	ClearNodes(Blueprint);
	RetireBlueprint(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameMemberSaveGameTest,
	"Cortex.Blueprint.RenameMember.SaveGame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameMemberSaveGameTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using namespace CortexRenameTest;
	// SaveGame variable: the rename needs the acknowledgment (I7).
	UBlueprint* Blueprint = MakeBlueprint(TEXT("RenameSaveGame"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AddVariable(Blueprint, TEXT("Score"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	{
		const FUnchanged Before(Blueprint);
		TSharedPtr<FJsonObject> DryParams = RenameParams(Blueprint, TEXT("variable"), TEXT("Score"), TEXT("Points"));
		DryParams->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult Dry = FCortexBPRenameMemberOps::Execute(DryParams);
		if (TestTrue(TEXT("dry_run succeeds"), Dry.bSuccess))
		{
			TestEqual(TEXT("required_acknowledgment"), ReadStrings(Dry.Data, TEXT("required_acknowledgment")), TArray<FString>{TEXT("save_game:Score")});
			TestEqual(TEXT("one finding"), Dry.Data->GetArrayField(TEXT("save_game_findings")).Num(), 1);
			const TArray<TSharedPtr<FJsonValue>> Findings = Dry.Data->GetArrayField(TEXT("save_game_findings"));
			if (Findings.Num() == 1)
			{
				TestEqual(TEXT("finding kind"), Findings[0]->AsObject()->GetStringField(TEXT("kind")), FString(TEXT("save_game_name_changed")));
			}
		}
		Before.Check(*this, Blueprint, TEXT("dry_run"));

		const FCortexCommandResult Refused = FCortexBPRenameMemberOps::Execute(RenameParams(Blueprint, TEXT("variable"), TEXT("Score"), TEXT("Points")));
		if (ExpectRefusal(*this, Refused, CortexErrorCodes::PotentialDataLoss, TEXT("SaveGame rename without acknowledgment")))
		{
			TestEqual(TEXT("refusal names the key"), ReadStrings(Refused.ErrorDetails, TEXT("required_acknowledgment")), TArray<FString>{TEXT("save_game:Score")});
		}
		Before.Check(*this, Blueprint, TEXT("unacknowledged"));

		TSharedPtr<FJsonObject> Stale = RenameParams(Blueprint, TEXT("variable"), TEXT("Score"), TEXT("Points"));
		SetStrings(Stale, TEXT("acknowledged_losses"), {TEXT("save_game:Other")});
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(Stale), CortexErrorCodes::PotentialDataLoss, TEXT("stale acknowledgment"));
		Before.Check(*this, Blueprint, TEXT("stale acknowledgment"));
	}

	TSharedPtr<FJsonObject> Acknowledged = RenameParams(Blueprint, TEXT("variable"), TEXT("Score"), TEXT("Points"));
	SetStrings(Acknowledged, TEXT("acknowledged_losses"), {TEXT("save_game:Score")});
	const FCortexCommandResult Applied = FCortexBPRenameMemberOps::Execute(Acknowledged);
	TestTrue(TEXT("acknowledged rename succeeds"), Applied.bSuccess);
	TestTrue(TEXT("renamed"), HasVariable(Blueprint, TEXT("Points")) && !HasVariable(Blueprint, TEXT("Score")));
	ClearNodes(Blueprint);
	RetireBlueprint(Blueprint);

	// FieldNotify variables refuse (M3).
	UBlueprint* Notify = MakeBlueprint(TEXT("RenameFieldNotify"), AActor::StaticClass());
	if (!TestNotNull(TEXT("FieldNotify Blueprint"), Notify))
	{
		return false;
	}
	AddVariable(Notify, TEXT("Health"), BoolPin());
	Notify->NewVariables.Last().SetMetaData(FBlueprintMetadata::MD_FieldNotify, TEXT("true"));
	FKismetEditorUtilities::CompileBlueprint(Notify);
	{
		const FUnchanged Before(Notify);
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(RenameParams(Notify, TEXT("variable"), TEXT("Health"), TEXT("Vitality"))),
			CortexErrorCodes::InvalidOperation, TEXT("FieldNotify variable"));
		Before.Check(*this, Notify, TEXT("FieldNotify"));
	}
	ClearNodes(Notify);
	RetireBlueprint(Notify);

	// An unloaded child that already declares the new name refuses through the derived classes of the scan (M4).
	FFixtures Fixtures;
	UBlueprint* Owner = MakeBlueprint(TEXT("BP_RenameOwner"), AActor::StaticClass(), TEXT("RenameOwner"));
	if (!TestNotNull(TEXT("Owner"), Owner))
	{
		return false;
	}
	AddVariable(Owner, TEXT("Gate"), BoolPin());
	FKismetEditorUtilities::CompileBlueprint(Owner);
	UBlueprint* Child = MakeBlueprint(TEXT("BP_RenameChild"), Owner->GeneratedClass, TEXT("RenameChild"));
	if (!TestNotNull(TEXT("Child"), Child))
	{
		RetireBlueprint(Owner);
		return false;
	}
	AddVariable(Child, TEXT("Door"), BoolPin());
	FKismetEditorUtilities::CompileBlueprint(Child);
	const FString ChildPath = Child->GetPathName();
	const bool bSaved = Fixtures.SaveAndTrack(Owner) && Fixtures.SaveAndTrack(Child);
	if (TestTrue(TEXT("Fixtures saved"), bSaved))
	{
		Fixtures.ScanSaved();
		Fixtures.Unload({Child}, *this);
		const FUnchanged Before(Owner);
		const FCortexCommandResult Refused = FCortexBPRenameMemberOps::Execute(RenameParams(Owner, TEXT("variable"), TEXT("Gate"), TEXT("Door")));
		if (ExpectRefusal(*this, Refused, CortexErrorCodes::NameCollision, TEXT("unloaded child declares the new name")))
		{
			TestTrue(TEXT("The child is named in the dependents"), HasPathInDependents(Refused.ErrorDetails, ChildPath));
		}
		Before.Check(*this, Owner, TEXT("unloaded child collision"));
	}
	Fixtures.Cleanup(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameMemberRefusalsTest,
	"Cortex.Blueprint.RenameMember.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameMemberRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using namespace CortexRenameTest;
	FFixtures Fixtures;
	UBlueprint* Owner = MakeBlueprint(TEXT("BP_RefuseOwner"), AActor::StaticClass(), TEXT("RefuseOwner"));
	if (!TestNotNull(TEXT("Owner"), Owner))
	{
		return false;
	}
	AddVariable(Owner, TEXT("bTarget"), BoolPin(), TEXT("true"));
	AddVariable(Owner, TEXT("bOther"), BoolPin());
	AddVariable(Owner, TEXT("Notified"), BoolPin());
	// (RepNotify is set below, after the compile: it names no real function.)
	USCS_Node* Component = Owner->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("MeshPart"));
	Owner->SimpleConstructionScript->AddNode(Component);
	FKismetEditorUtilities::CompileBlueprint(Owner);

	UBlueprint* LoadedChild = MakeBlueprint(TEXT("BP_RefuseLoadedChild"), Owner->GeneratedClass, TEXT("RefuseLoadedChild"));
	if (!TestNotNull(TEXT("Loaded child"), LoadedChild))
	{
		RetireBlueprint(Owner);
		return false;
	}
	AddVariable(LoadedChild, TEXT("bClash"), BoolPin());
	FKismetEditorUtilities::CompileBlueprint(LoadedChild);

	// The RepNotify function is declared after the last compile of the owner (it names no real function).
	Owner->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Owner, TEXT("Notified"))].RepNotifyFunc = TEXT("OnRep_Notified");
	TUniquePtr<FUnchanged> Before = MakeUnique<FUnchanged>(Owner);
	const auto Refuse = [&](const TSharedPtr<FJsonObject>& Params, const FString& Code, const TCHAR* Label) -> FCortexCommandResult
	{
		const FCortexCommandResult Result = FCortexBPRenameMemberOps::Execute(Params);
		ExpectRefusal(*this, Result, Code, Label);
		Before->Check(*this, Owner, Label);
		return Result;
	};

	Refuse(RenameParams(Owner, TEXT("variable"), TEXT("bTarget"), TEXT("bad.name")), CortexErrorCodes::InvalidField, TEXT("invalid identifier"));
	Refuse(RenameParams(Owner, TEXT("variable"), TEXT("bTarget"), TEXT("bTarget")), CortexErrorCodes::InvalidField, TEXT("same name"));
	Refuse(RenameParams(Owner, TEXT("variable"), TEXT("bMissing"), TEXT("bFresh")), CortexErrorCodes::VariableNotFound, TEXT("missing variable"));
	Refuse(RenameParams(Owner, TEXT("variable"), TEXT("bTarget"), TEXT("bOther")), CortexErrorCodes::NameCollision, TEXT("target exists in the Blueprint"));
	Refuse(RenameParams(Owner, TEXT("variable"), TEXT("bTarget"), TEXT("InitialLifeSpan")), CortexErrorCodes::NameCollision, TEXT("target exists on the native parent"));
	Refuse(RenameParams(Owner, TEXT("variable"), TEXT("bTarget"), TEXT("bClash")), CortexErrorCodes::NameCollision, TEXT("target exists as a variable in a loaded child"));
	Refuse(RenameParams(Owner, TEXT("variable"), TEXT("Notified"), TEXT("Informed")), CortexErrorCodes::InvalidOperation, TEXT("RepNotify variable"));
	const FCortexCommandResult ComponentRefusal = Refuse(RenameParams(Owner, TEXT("variable"), TEXT("MeshPart"), TEXT("MeshOther")), CortexErrorCodes::InvalidOperation, TEXT("SCS component variable"));
	TestTrue(TEXT("Component refusal points at rename_scs_component"), ComponentRefusal.ErrorMessage.Contains(TEXT("rename_scs_component")));
	Refuse(RenameParams(Owner, TEXT("local"), TEXT("bTarget"), TEXT("bFresh")), CortexErrorCodes::InvalidField, TEXT("local without function"));

	// A dry run reports the blocker instead of refusing.
	{
		TSharedPtr<FJsonObject> Dry = RenameParams(Owner, TEXT("variable"), TEXT("bTarget"), TEXT("bOther"));
		Dry->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult Result = FCortexBPRenameMemberOps::Execute(Dry);
		if (TestTrue(TEXT("dry_run succeeds with a blocker"), Result.bSuccess) && Result.Data.IsValid())
		{
			TestFalse(TEXT("can_apply false"), Result.Data->GetBoolField(TEXT("can_apply")));
			TestEqual(TEXT("one blocker"), Result.Data->GetArrayField(TEXT("blocking")).Num(), 1);
		}
		Before->Check(*this, Owner, TEXT("blocked dry_run"));
	}

	// Stale fingerprint.
	{
		TSharedPtr<FJsonObject> Stale = RenameParams(Owner, TEXT("variable"), TEXT("bTarget"), TEXT("bFresh"));
		const TSharedPtr<FJsonObject> Fingerprint = FCortexBPMigrationGuard::ComputeFingerprint(Owner);
		Fingerprint->SetStringField(TEXT("migration_state_hash"), TEXT("0000"));
		Stale->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
		Refuse(Stale, CortexErrorCodes::StalePrecondition, TEXT("stale fingerprint"));
	}

	// Dependents that reference the variable: one saved and unloaded (registry), one loaded and never saved.
	UBlueprint* Saved = MakeBlueprint(TEXT("BP_RefuseSaved"), AActor::StaticClass(), TEXT("RefuseSaved"));
	UBlueprint* Hot = MakeBlueprint(TEXT("BP_RefuseHot"), AActor::StaticClass(), TEXT("RefuseHot"));
	if (!TestNotNull(TEXT("Saved referencer"), Saved) || !TestNotNull(TEXT("Hot referencer"), Hot))
	{
		RetireBlueprint(LoadedChild);
		RetireBlueprint(Owner);
		return false;
	}
	for (UBlueprint* Referencer : {Saved, Hot})
	{
		FEdGraphPinType HolderType;
		HolderType.PinCategory = UEdGraphSchema_K2::PC_Object;
		HolderType.PinSubCategoryObject = Owner->GeneratedClass;
		FBlueprintEditorUtils::AddMemberVariable(Referencer, TEXT("Holder"), HolderType);
		AddGet(FBlueprintEditorUtils::FindEventGraph(Referencer), TEXT("bTarget"), Owner->GeneratedClass);
	}
	const FString SavedPath = Saved->GetPathName();
	const FString HotPath = Hot->GetPathName();

	// The loaded-but-unsaved referencer is the only dependent so far.
	{
		const FCortexCommandResult Result = Refuse(RenameParams(Owner, TEXT("variable"), TEXT("bTarget"), TEXT("bFresh")), CortexErrorCodes::InvalidOperation, TEXT("loaded unsaved dependent"));
		TestTrue(TEXT("The unsaved referencer is listed"), HasPathInDependents(Result.ErrorDetails, HotPath));
	}

	// Everything saved, then the registry-only dependent unloaded.
	Fixtures.Track(LoadedChild);
	const bool bSaved = Fixtures.SaveAndTrack(Owner) && Fixtures.SaveAndTrack(Saved);
	if (TestTrue(TEXT("Fixtures saved"), bSaved))
	{
		Fixtures.Track(Hot);
		Fixtures.ScanSaved();
		Before = MakeUnique<FUnchanged>(Owner);
		Fixtures.Unload({Saved, Hot}, *this);
		const FCortexCommandResult Result = Refuse(RenameParams(Owner, TEXT("variable"), TEXT("bTarget"), TEXT("bFresh")), CortexErrorCodes::InvalidOperation, TEXT("unloaded dependent"));
		TestTrue(TEXT("The registry referencer is listed"), HasPathInDependents(Result.ErrorDetails, SavedPath));
		const TSharedPtr<FJsonObject>* References = nullptr;
		TestTrue(TEXT("The refusal carries references"), Result.ErrorDetails.IsValid() && Result.ErrorDetails->TryGetObjectField(TEXT("references"), References));
	}

	Fixtures.Cleanup(*this);
	return true;
}
