// retarget_calls with split pins: a struct output shown as its members (a split pin) is retargeted when the replacement pin carries the same
// struct type - the replacement is split, the members map by name and every member link is re-made and verified by (node GUID, pin name);
// a replacement with different members is blocked. A failed apply splits the old pins again and restores every PinId and link exactly.

#include "Misc/AutomationTest.h"

#include "CortexGraphRetargetTestSupport.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace
{
	using namespace CortexGraphRetargetTest;

	/** A BPI whose GetState returns a vector named State. */
	UBlueprint* MakeStateInterface(FScope& Scope, const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s_%s"), Name, *UniqueSuffix()));
		UBlueprint* Interface = FKismetEditorUtilities::CreateBlueprint(
			UInterface::StaticClass(), Package, FName(Name), BPTYPE_Interface, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (Interface == nullptr)
		{
			return nullptr;
		}
		Scope.Blueprints.Add(Interface);
		UEdGraph* const Graph = CortexGraphReplaceInterfaceTest::AddInterfaceFunction(Interface, TEXT("GetState"), /*bWithOutput=*/false);
		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* Node : Graph ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			if (Entry == nullptr) { Entry = Cast<UK2Node_FunctionEntry>(Node); }
		}
		if (Entry == nullptr)
		{
			return nullptr;
		}
		FEdGraphPinType VectorType;
		VectorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		VectorType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		UK2Node_FunctionResult* const Result = NewObject<UK2Node_FunctionResult>(Graph);
		Result->FunctionReference = Entry->FunctionReference;
		Result->CreateNewGuid();
		Result->AllocateDefaultPins();
		Graph->AddNode(Result, true, false);
		Result->CreateUserDefinedPin(TEXT("State"), VectorType, EGPD_Input, false);
		FKismetEditorUtilities::CompileBlueprint(Interface);
		return Interface;
	}

	struct FSplitCaller
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Events = nullptr;
		UK2Node_Event* BeginPlay = nullptr;
		UK2Node_Self* Self = nullptr;
		UEdGraphNode* Source = nullptr;
		TArray<UK2Node_VariableSet*> Setters;
	};

	/**
	 * BeginPlay -> GetState (a message, or a typed call on a cast to the BPI) whose State output is split into X/Y/Z, each member feeding a
	 * variable setter. Compiled.
	 */
	bool MakeSplitCaller(FScope& Scope, UBlueprint* Interface, const TCHAR* Name, const bool bTyped, FSplitCaller& Out)
	{
		Out.Blueprint = MakeActorBlueprint(Scope, Name);
		if (Out.Blueprint == nullptr)
		{
			return false;
		}
		const UEdGraphSchema_K2* const Schema = GetDefault<UEdGraphSchema_K2>();
		FEdGraphPinType DoubleType;
		DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
		DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		for (const TCHAR* Variable : { TEXT("OutX"), TEXT("OutY"), TEXT("OutZ") })
		{
			if (!FBlueprintEditorUtils::AddMemberVariable(Out.Blueprint, Variable, DoubleType)) return false;
		}
		UClass* const Old = Interface->GeneratedClass;
		Out.Events = FBlueprintEditorUtils::FindEventGraph(Out.Blueprint);
		Out.BeginPlay = AddBeginPlay(Out.Events);
		Out.Self = AddNode<UK2Node_Self>(Out.Events);
		if (!bTyped)
		{
			Out.Source = AddMessage(Out.Events, Old, TEXT("GetState"));
			if (!Connect(Out.BeginPlay, UEdGraphSchema_K2::PN_Then, Out.Source, UEdGraphSchema_K2::PN_Execute)
				|| !Connect(Out.Self, UEdGraphSchema_K2::PN_Self, Out.Source, UEdGraphSchema_K2::PN_Self))
			{
				return false;
			}
		}
		else
		{
			UK2Node_DynamicCast* const Cast = AddNode<UK2Node_DynamicCast>(Out.Events, [&](UK2Node_DynamicCast* Node) { Node->TargetType = Old; Node->SetPurity(false); });
			Out.Source = AddNode<UK2Node_CallFunction>(Out.Events, [&](UK2Node_CallFunction* Node) { Node->FunctionReference.SetExternalMember(TEXT("GetState"), Old); });
			if (!Cast->GetValidCastPin() || !Cast->GetCastResultPin()
				|| !Connect(Out.BeginPlay, UEdGraphSchema_K2::PN_Then, Cast, UEdGraphSchema_K2::PN_Execute)
				|| !Connect(Out.Self, UEdGraphSchema_K2::PN_Self, Cast, UEdGraphSchema_K2::PN_ObjectToCast)
				|| !Schema->TryCreateConnection(Cast->GetValidCastPin(), Out.Source->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
				|| !Schema->TryCreateConnection(Cast->GetCastResultPin(), Out.Source->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input)))
			{
				return false;
			}
		}
		UEdGraphPin* const State = Out.Source->FindPin(TEXT("State"), EGPD_Output);
		if (State == nullptr)
		{
			return false;
		}
		Schema->SplitPin(State, false);
		UEdGraphNode* Previous = Out.Source;
		for (const TCHAR* Axis : { TEXT("X"), TEXT("Y"), TEXT("Z") })
		{
			const FString Variable = FString(TEXT("Out")) + Axis;
			UK2Node_VariableSet* const Setter = AddNode<UK2Node_VariableSet>(Out.Events, [&](UK2Node_VariableSet* Node) { Node->VariableReference.SetSelfMember(FName(*Variable)); });
			if (!Connect(Previous, UEdGraphSchema_K2::PN_Then, Setter, UEdGraphSchema_K2::PN_Execute)
				|| !Connect(Out.Source, FName(*(FString(TEXT("State_")) + Axis)), Setter, FName(*Variable)))
			{
				return false;
			}
			Out.Setters.Add(Setter);
			Previous = Setter;
		}
		FKismetEditorUtilities::CompileBlueprint(Out.Blueprint);
		return Out.Blueprint->Status != BS_Error;
	}

	TSharedPtr<FJsonObject> StateMigration(UBlueprint* Interface, const UClass* To, const TArray<TSharedPtr<FJsonValue>>& PinMap, const TArray<TSharedPtr<FJsonValue>>* Sites,
		const TCHAR* ToFunction = TEXT("GetState"))
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("op"), TEXT("retarget_calls"));
		TSharedPtr<FJsonObject> FromJson = MakeShared<FJsonObject>();
		FromJson->SetStringField(TEXT("owner"), Interface->GetPathName());
		FromJson->SetStringField(TEXT("function"), TEXT("GetState"));
		Json->SetObjectField(TEXT("from"), FromJson);
		TSharedPtr<FJsonObject> ToJson = MakeShared<FJsonObject>();
		ToJson->SetStringField(TEXT("owner"), To->GetPathName());
		ToJson->SetStringField(TEXT("function"), ToFunction);
		Json->SetObjectField(TEXT("to"), ToJson);
		Json->SetArrayField(TEXT("pin_map"), PinMap);
		if (Sites != nullptr)
		{
			Json->SetArrayField(TEXT("sites"), *Sites);
		}
		return Json;
	}

	TArray<TSharedPtr<FJsonValue>> StatePinMap()
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("from_pin"), TEXT("State"));
		Entry->SetStringField(TEXT("to_pin"), TEXT("ReturnValue"));
		TArray<TSharedPtr<FJsonValue>> Map;
		Map.Add(MakeShared<FJsonValueObject>(Entry));
		return Map;
	}

	bool PreviewState(UBlueprint* Blueprint, UBlueprint* Interface, const UClass* To, const TCHAR* ToFunction, FCortexGraphPreparedPatch& OutPreview, FCortexCommandResult& OutError)
	{
		return FCortexGraphPatchOps::Preflight(Blueprint, Request(Blueprint, StateMigration(Interface, To, StatePinMap(), nullptr, ToFunction)), OutPreview, OutError);
	}

	TSharedPtr<FJsonObject> ApplyState(UBlueprint* Blueprint, UBlueprint* Interface, const UClass* To, const FCortexGraphPreparedPatch& Preview)
	{
		const TArray<TSharedPtr<FJsonValue>> Sites = Preview.RetargetPlan->GetArrayField(TEXT("sites"));
		TSharedPtr<FJsonObject> Apply = Request(Blueprint, StateMigration(Interface, To, StatePinMap(), &Sites), /*bDryRun=*/false);
		Apply->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
		return Apply;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationRetargetSplitPinsTest,
	"Cortex.Graph.Authoring.Migration.Retarget.SplitPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetargetSplitPinsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphRetargetTest;

	FScope Scope;
	UBlueprint* const Interface = MakeStateInterface(Scope, TEXT("BPI_Issue122RetargetSplit"));
	if (!TestNotNull(TEXT("State BPI"), Interface))
	{
		return false;
	}
	UClass* const Native = UCortexIssue122SplitInterface::StaticClass();

	for (const bool bTyped : { false, true })
	{
		const FString Label = bTyped ? TEXT("typed_call") : TEXT("message");
		FSplitCaller Caller;
		if (!TestTrue(*FString::Printf(TEXT("[%s] split caller"), *Label),
			MakeSplitCaller(Scope, Interface, bTyped ? TEXT("BP_Issue122RetargetSplitTyped") : TEXT("BP_Issue122RetargetSplitMessage"), bTyped, Caller)))
		{
			return false;
		}
		const FGuid SourceGuid = Caller.Source->NodeGuid;
		TestEqual(*FString::Printf(TEXT("[%s] the source pin is split into three members"), *Label), Caller.Source->FindPin(TEXT("State"), EGPD_Output)->SubPins.Num(), 3);

		FCortexGraphPreparedPatch Preview1;
		FCortexCommandResult PreviewError;
		const bool bPreviewed = PreviewState(Caller.Blueprint, Interface, Native, TEXT("GetState"), Preview1, PreviewError);
		TestTrue(*FString::Printf(TEXT("[%s] the preview succeeds [%s]"), *Label, *PreviewError.ErrorMessage), bPreviewed);
		if (!bPreviewed)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Site = SiteByNode(Preview1, SourceGuid);
		if (TestTrue(*FString::Printf(TEXT("[%s] the site is published"), *Label), Site.IsValid()))
		{
			TestFalse(*FString::Printf(TEXT("[%s] the split site is not blocked [%s]"), *Label, *Site->GetStringField(TEXT("blocked"))), Site->HasField(TEXT("blocked")));
			const TSharedPtr<FJsonObject> Member = PinOfSite(Site, TEXT("State_X"), TEXT("output"));
			if (TestTrue(*FString::Printf(TEXT("[%s] the member pin is published"), *Label), Member.IsValid()))
			{
				TestEqual(*FString::Printf(TEXT("[%s] ... with its parent"), *Label), Member->GetStringField(TEXT("parent")), FString(TEXT("State")));
				TestEqual(*FString::Printf(TEXT("[%s] ... mapped under the replacement pin"), *Label), Member->GetStringField(TEXT("maps_to")), FString(TEXT("ReturnValue_X")));
				TestEqual(*FString::Printf(TEXT("[%s] ... with its link"), *Label), Member->GetArrayField(TEXT("links")).Num(), 1);
			}
			TestTrue(*FString::Printf(TEXT("[%s] the parent pin is published, unlinked"), *Label),
				PinOfSite(Site, TEXT("State"), TEXT("output")).IsValid() && PinOfSite(Site, TEXT("State"), TEXT("output"))->GetArrayField(TEXT("links")).IsEmpty());
		}

		FCompileCounter Counter;
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		const bool bApplied = FCortexGraphPatchOps::Execute(Caller.Blueprint, ApplyState(Caller.Blueprint, Interface, Native, Preview1), Outcome, Error);
		TestTrue(FString::Printf(TEXT("[%s] the apply succeeds [%s: %s] [%s]"), *Label, *Error.ErrorCode, *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))), bApplied);
		if (!bApplied)
		{
			return false;
		}
		TestEqual(*FString::Printf(TEXT("[%s] readback matched"), *Label), Outcome.ReadbackStatus, FString(TEXT("matched")));
		TestEqual(*FString::Printf(TEXT("[%s] one target compile"), *Label), Counter.TargetCompiles, 1);
		TestTrue(*FString::Printf(TEXT("[%s] the asset compiles"), *Label), Caller.Blueprint->Status == BS_UpToDate);
		TestTrue(*FString::Printf(TEXT("[%s] the node keeps its GUID"), *Label), Caller.Source->NodeGuid == SourceGuid);
		TestNull(*FString::Printf(TEXT("[%s] the old output is gone"), *Label), Caller.Source->FindPin(TEXT("State"), EGPD_Output));
		UEdGraphPin* const Parent = Caller.Source->FindPin(TEXT("ReturnValue"), EGPD_Output);
		if (TestNotNull(*FString::Printf(TEXT("[%s] the replacement output exists"), *Label), Parent))
		{
			TestEqual(*FString::Printf(TEXT("[%s] ... split into three members"), *Label), Parent->SubPins.Num(), 3);
			TestTrue(*FString::Printf(TEXT("[%s] ... hidden like a split pin"), *Label), Parent->bHidden);
		}
		const TCHAR* Axes[] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
		for (int32 Index = 0; Index < 3; ++Index)
		{
			TestTrue(*FString::Printf(TEXT("[%s] member %s reaches its setter"), *Label, Axes[Index]),
				LinkedPins(Caller.Source, FName(*(FString(TEXT("ReturnValue_")) + Axes[Index])), Caller.Setters[Index], FName(*(FString(TEXT("Out")) + Axes[Index]))));
		}
		FString FirstRemaining;
		TestEqual(*FString::Printf(TEXT("[%s] no site of the old endpoint remains"), *Label),
			FCortexGraphRetargetOps::CountRemainingSites(Caller.Blueprint, Interface->GeneratedClass, TEXT("GetState"), FirstRemaining), 0);
	}

	// A replacement with different members is blocked, not retargeted.
	{
		FSplitCaller Caller;
		if (!TestTrue(TEXT("[mismatch] split caller"), MakeSplitCaller(Scope, Interface, TEXT("BP_Issue122RetargetSplitMismatch"), false, Caller)))
		{
			return false;
		}
		const FString Before = Snapshot(Caller.Blueprint);
		FCortexGraphPreparedPatch Preview1;
		FCortexCommandResult PreviewError;
		TestTrue(FString::Printf(TEXT("[mismatch] the preview is published [%s]"), *PreviewError.ErrorMessage),
			PreviewState(Caller.Blueprint, Interface, Native, TEXT("GetRotation"), Preview1, PreviewError));
		const TSharedPtr<FJsonObject> Site = SiteByNode(Preview1, Caller.Source->NodeGuid);
		TestTrue(TEXT("[mismatch] the site is blocked as split_pin"), Site.IsValid() && Site->GetStringField(TEXT("blocked")).Contains(TEXT("split_pin")));
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		TestFalse(TEXT("[mismatch] the apply is refused"), FCortexGraphPatchOps::Execute(Caller.Blueprint, ApplyState(Caller.Blueprint, Interface, Native, Preview1), Outcome, Error));
		TestEqual(TEXT("[mismatch] ... without applying"), Outcome.ApplyStatus, FString(TEXT("not_requested")));
		TestEqual(TEXT("[mismatch] ... and the asset is exactly as it was"), Snapshot(Caller.Blueprint), Before);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationRetargetSplitRollbackTest,
	"Cortex.Graph.Authoring.Migration.Retarget.SplitPinsRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetargetSplitRollbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphRetargetTest;

	FScope Scope;
	UBlueprint* const Interface = MakeStateInterface(Scope, TEXT("BPI_Issue122RetargetSplitRollback"));
	FSplitCaller Caller;
	if (!TestNotNull(TEXT("State BPI"), Interface) || !TestTrue(TEXT("Split caller"), MakeSplitCaller(Scope, Interface, TEXT("BP_Issue122RetargetSplitRollback"), false, Caller)))
	{
		return false;
	}
	UClass* const Native = UCortexIssue122SplitInterface::StaticClass();
	const FString Before = Snapshot(Caller.Blueprint);

	struct FFault { const TCHAR* Label; const TCHAR* Apply; const TCHAR* Readback; };
	const FFault Faults[] = {
		{ TEXT("after the first site"), TEXT("migration_retarget_after_first_site"), nullptr },
		{ TEXT("after every site"), TEXT("migration_retarget_after_sites"), nullptr },
		{ TEXT("readback link divergence"), nullptr, TEXT("retarget_links") },
	};
	for (const FFault& Fault : Faults)
	{
		FCortexGraphPreparedPatch Preview1;
		FCortexCommandResult PreviewError;
		if (!TestTrue(FString::Printf(TEXT("[%s] preview"), Fault.Label), PreviewState(Caller.Blueprint, Interface, Native, TEXT("GetState"), Preview1, PreviewError)))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Apply = ApplyState(Caller.Blueprint, Interface, Native, Preview1);
		FCompileCounter Counter;
		if (Fault.Apply != nullptr) { FCortexGraphPatchOps::SetApplyFaultPointForTesting(FName(Fault.Apply)); }
		if (Fault.Readback != nullptr) { FCortexGraphPatchOps::SetReadbackFaultForTesting(FName(Fault.Readback)); }
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		const bool bApplied = FCortexGraphPatchOps::Execute(Caller.Blueprint, Apply, Outcome, Error);
		FCortexGraphPatchOps::ClearApplyFaultPointForTesting();
		FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);

		TestFalse(FString::Printf(TEXT("[%s] the apply fails"), Fault.Label), bApplied);
		TestEqual(FString::Printf(TEXT("[%s] rollback restored [%s]"), Fault.Label, *FString::Join(Outcome.Diagnostics, TEXT("; "))), Outcome.RollbackStatus, FString(TEXT("restored")));
		TestEqual(FString::Printf(TEXT("[%s] exactly one recovery compile"), Fault.Label), Counter.RecoveryCompiles, 1);
		TestEqual(FString::Printf(TEXT("[%s] the split pin, its members, PinIds and links are exactly restored"), Fault.Label), Snapshot(Caller.Blueprint), Before);
		TestEqual(FString::Printf(TEXT("[%s] the source pin is split again"), Fault.Label), Caller.Source->FindPin(TEXT("State"), EGPD_Output) ? Caller.Source->FindPin(TEXT("State"), EGPD_Output)->SubPins.Num() : -1, 3);
	}
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
