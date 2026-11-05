#include "Misc/AutomationTest.h"

#include "CortexBPRenameMemberTestSupport.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "GameFramework/Actor.h"
#include "K2Node_ActorBoundEvent.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPRenameMemberOps.h"


namespace CortexRenameMemberFunctionTestLocal
{
	using namespace CortexRenameTest;

	UK2Node_CallFunction* AddSelfCall(UEdGraph* Graph, const TCHAR* Function)
	{
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
		UK2Node_CallFunction* Node = Creator.CreateNode();
		Node->FunctionReference.SetSelfMember(FName(Function));
		Creator.Finalize();
		return Node;
	}

	struct FPostApplyHookScope
	{
		explicit FPostApplyHookScope(TFunction<void(UBlueprint*)> Hook)
		{
			FCortexBPRenameMemberOps::SetPostApplyHookForTest(MoveTemp(Hook));
		}
		~FPostApplyHookScope()
		{
			FCortexBPRenameMemberOps::ClearPostApplyHookForTest();
		}
	};

	struct FSkipUndoScope
	{
		FSkipUndoScope() { FCortexBPGuardedRun::SetSkipUndoForTest(true); }
		~FSkipUndoScope() { FCortexBPGuardedRun::SetSkipUndoForTest(false); }
	};

	UEdGraph* FunctionGraphNamed(const UBlueprint* Blueprint, const TCHAR* Name)
	{
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph->GetFName() == FName(Name))
			{
				return Graph;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameMemberFunctionTest,
	"Cortex.Blueprint.RenameMember.Function",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameMemberFunctionTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using namespace CortexRenameTest;
	using namespace CortexRenameMemberFunctionTestLocal;
	UBlueprint* Blueprint = MakeBlueprint(TEXT("RenameFunction"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	UEdGraph* Target = AddFunctionGraph(Blueprint, TEXT("ComputeThing"));
	UEdGraph* Other = AddFunctionGraph(Blueprint, TEXT("Caller"));
	if (!TestNotNull(TEXT("Function graphs"), Target) || !TestNotNull(TEXT("Second function graph"), Other))
	{
		Discard(Blueprint);
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UK2Node_CallFunction* EventCall = AddSelfCall(EventGraph, TEXT("ComputeThing"));
	UK2Node_CallFunction* OtherCall = AddSelfCall(Other, TEXT("ComputeThing"));
	TestTrue(TEXT("Chained call"), Connect(EntryOf(Other)->FindPin(UEdGraphSchema_K2::PN_Then), OtherCall->GetExecPin()));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("Fixture compiles"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

	{
		const FUnchanged Before(Blueprint);
		TSharedPtr<FJsonObject> Dry = RenameParams(Blueprint, TEXT("function"), TEXT("ComputeThing"), TEXT("ComputeValue"));
		Dry->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult DryResult = FCortexBPRenameMemberOps::Execute(Dry);
		if (TestTrue(TEXT("dry_run succeeds"), DryResult.bSuccess))
		{
			TestEqual(TEXT("dry_run lists both call nodes"), CountAt(DryResult.Data, TEXT("references"), TEXT("before")), 2);
		}
		Before.Check(*this, Blueprint, TEXT("function dry_run"));
	}

	const FCortexCommandResult Result = FCortexBPRenameMemberOps::Execute(RenameParams(Blueprint, TEXT("function"), TEXT("ComputeThing"), TEXT("ComputeValue")));
	if (TestTrue(TEXT("function rename succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestTrue(TEXT("renamed"), Result.Data->GetBoolField(TEXT("renamed")));
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestEqual(TEXT("compile clean"), Result.Data->GetStringField(TEXT("compile_status")), FString(TEXT("success")));
		TestEqual(TEXT("references after"), CountAt(Result.Data, TEXT("references"), TEXT("after")), 2);
	}
	else if (!Result.bSuccess)
	{
		AddError(FString::Printf(TEXT("function rename failed: %s %s"), *Result.ErrorCode, *Result.ErrorMessage));
	}

	TestTrue(TEXT("Graph renamed in place"), FunctionGraphNamed(Blueprint, TEXT("ComputeValue")) == Target);
	TestNull(TEXT("Old graph name gone"), FunctionGraphNamed(Blueprint, TEXT("ComputeThing")));
	TestEqual(TEXT("Event graph call follows"), EventCall->FunctionReference.GetMemberName(), FName(TEXT("ComputeValue")));
	TestEqual(TEXT("Function graph call follows"), OtherCall->FunctionReference.GetMemberName(), FName(TEXT("ComputeValue")));
	TestTrue(TEXT("Calls stay self context"), EventCall->FunctionReference.IsSelfContext() && OtherCall->FunctionReference.IsSelfContext());
	if (const UK2Node_FunctionEntry* Entry = EntryOf(Target))
	{
		TestEqual(TEXT("Entry node FunctionReference follows"), Entry->FunctionReference.GetMemberName(), FName(TEXT("ComputeValue")));
	}
	TestTrue(TEXT("Compiled class has the new function"),
		Blueprint->GeneratedClass != nullptr && Blueprint->GeneratedClass->FindFunctionByName(TEXT("ComputeValue")) != nullptr
		&& Blueprint->GeneratedClass->FindFunctionByName(TEXT("ComputeThing")) == nullptr);

	// Override graphs refuse.
	{
		UEdGraph* Construction = FunctionGraphNamed(Blueprint, TEXT("UserConstructionScript"));
		if (TestNotNull(TEXT("UserConstructionScript override graph"), Construction))
		{
			const FUnchanged Before(Blueprint);
			ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(RenameParams(Blueprint, TEXT("function"), TEXT("UserConstructionScript"), TEXT("BuildIt"))),
				CortexErrorCodes::InvalidOperation, TEXT("override graph"));
			Before.Check(*this, Blueprint, TEXT("override"));
		}
	}

	// Collision with a native UFunction refuses.
	{
		const FUnchanged Before(Blueprint);
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(RenameParams(Blueprint, TEXT("function"), TEXT("ComputeValue"), TEXT("K2_GetActorLocation"))),
			CortexErrorCodes::NameCollision, TEXT("native function collision"));
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(RenameParams(Blueprint, TEXT("function"), TEXT("Missing"), TEXT("Fresh"))),
			CortexErrorCodes::GraphNotFound, TEXT("missing function"));
		Before.Check(*this, Blueprint, TEXT("function refusals"));
	}

	// A loaded dependent (never saved) that calls the function refuses.
	UBlueprint* Caller = MakeBlueprint(TEXT("BP_RenameFnCaller"), AActor::StaticClass(), TEXT("RenameFnCaller"));
	if (TestNotNull(TEXT("Caller"), Caller))
	{
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*FBlueprintEditorUtils::FindEventGraph(Caller));
		UK2Node_CallFunction* External = Creator.CreateNode();
		External->FunctionReference.SetExternalMember(TEXT("ComputeValue"), Blueprint->GeneratedClass);
		Creator.Finalize();

		const FUnchanged Before(Blueprint);
		const FCortexCommandResult Refused = FCortexBPRenameMemberOps::Execute(RenameParams(Blueprint, TEXT("function"), TEXT("ComputeValue"), TEXT("ComputeAgain")));
		if (ExpectRefusal(*this, Refused, CortexErrorCodes::InvalidOperation, TEXT("loaded dependent calls the function")))
		{
			TestTrue(TEXT("The caller is listed"), HasPathInDependents(Refused.ErrorDetails, Caller->GetPathName()));
		}
		Before.Check(*this, Blueprint, TEXT("dependent caller"));
		ClearNodes(Caller);
		Discard(Caller);
	}

	// An interface-implementing graph refuses (state injected on a throwaway Blueprint; it is never compiled).
	UBlueprint* Implementer = MakeBlueprint(TEXT("RenameInterfaceGraph"), AActor::StaticClass());
	if (TestNotNull(TEXT("Implementer"), Implementer))
	{
		UEdGraph* InterfaceGraph = FBlueprintEditorUtils::CreateNewGraph(
			Implementer, TEXT("Ping"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBPInterfaceDescription Description;
		Description.Interface = UInterface::StaticClass();
		Description.Graphs.Add(InterfaceGraph);
		Implementer->ImplementedInterfaces.Add(Description);
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(RenameParams(Implementer, TEXT("function"), TEXT("Ping"), TEXT("Pong"))),
			CortexErrorCodes::InvalidOperation, TEXT("interface graph"));
		TestEqual(TEXT("Interface graph keeps its name"), InterfaceGraph->GetFName(), FName(TEXT("Ping")));
		Implementer->ImplementedInterfaces.Reset();
		ClearNodes(Implementer);
		Discard(Implementer);
	}

	ClearNodes(Blueprint);
	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameMemberLocalTest,
	"Cortex.Blueprint.RenameMember.Local",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameMemberLocalTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using namespace CortexRenameTest;
	using namespace CortexRenameMemberFunctionTestLocal;
	UBlueprint* Blueprint = MakeBlueprint(TEXT("RenameLocal"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AddVariable(Blueprint, TEXT("Counter"), BoolPin(), TEXT("true"));
	UEdGraph* Calc = AddFunctionGraph(Blueprint, TEXT("Calc"));
	UEdGraph* Mixed = AddFunctionGraph(Blueprint, TEXT("Mixed"));
	if (!TestNotNull(TEXT("Function graphs"), Calc) || !TestNotNull(TEXT("Mixed graph"), Mixed))
	{
		Discard(Blueprint);
		return false;
	}
	TestTrue(TEXT("Local added to Calc"), FBlueprintEditorUtils::AddLocalVariable(Blueprint, Calc, TEXT("Counter"), BoolPin(), TEXT("true")));
	TestTrue(TEXT("Local added to Mixed"), FBlueprintEditorUtils::AddLocalVariable(Blueprint, Mixed, TEXT("Counter"), BoolPin()));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	UK2Node_VariableGet* LocalGet = AddGet(Calc, TEXT("Counter"), nullptr, TEXT("Calc"));
	UK2Node_VariableSet* LocalSet = AddSet(Calc, TEXT("Counter"), TEXT("Calc"));
	TestTrue(TEXT("Local nodes wired"), WireGetSet(Calc, LocalGet, LocalSet));
	UK2Node_VariableGet* MemberGet = AddGet(FBlueprintEditorUtils::FindEventGraph(Blueprint), TEXT("Counter"));
	UK2Node_VariableGet* MixedMemberGet = AddGet(Mixed, TEXT("Counter"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("Fixture compiles"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

	const auto LocalParams = [&](const TCHAR* Function, const TCHAR* Old, const TCHAR* New)
	{
		TSharedPtr<FJsonObject> Params = RenameParams(Blueprint, TEXT("local"), Old, New);
		if (Function != nullptr)
		{
			Params->SetStringField(TEXT("function"), Function);
		}
		return Params;
	};

	// Refusals first: nothing changes.
	{
		const FUnchanged Before(Blueprint);
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(LocalParams(nullptr, TEXT("Counter"), TEXT("Count2"))),
			CortexErrorCodes::InvalidField, TEXT("missing function param"));
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(LocalParams(TEXT("Nowhere"), TEXT("Counter"), TEXT("Count2"))),
			CortexErrorCodes::GraphNotFound, TEXT("unknown function"));
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(LocalParams(TEXT("Calc"), TEXT("Absent"), TEXT("Count2"))),
			CortexErrorCodes::VariableNotFound, TEXT("unknown local"));
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(LocalParams(TEXT("Mixed"), TEXT("Counter"), TEXT("Count2"))),
			CortexErrorCodes::InvalidOperation, TEXT("member nodes of the same name inside the function"));
		TSharedPtr<FJsonObject> WithFunction = RenameParams(Blueprint, TEXT("variable"), TEXT("Counter"), TEXT("Count2"));
		WithFunction->SetStringField(TEXT("function"), TEXT("Calc"));
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(WithFunction), CortexErrorCodes::InvalidField, TEXT("function with kind variable"));
		Before.Check(*this, Blueprint, TEXT("local refusals"));
	}

	// dry_run lists the local nodes only.
	{
		const FUnchanged Before(Blueprint);
		TSharedPtr<FJsonObject> Dry = LocalParams(TEXT("Calc"), TEXT("Counter"), TEXT("Count2"));
		Dry->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult DryResult = FCortexBPRenameMemberOps::Execute(Dry);
		if (TestTrue(TEXT("dry_run succeeds"), DryResult.bSuccess))
		{
			TestEqual(TEXT("dry_run lists the two local nodes"), CountAt(DryResult.Data, TEXT("references"), TEXT("before")), 2);
		}
		Before.Check(*this, Blueprint, TEXT("local dry_run"));
	}

	const FCortexCommandResult Result = FCortexBPRenameMemberOps::Execute(LocalParams(TEXT("Calc"), TEXT("Counter"), TEXT("Count2")));
	if (TestTrue(TEXT("local rename succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestTrue(TEXT("renamed"), Result.Data->GetBoolField(TEXT("renamed")));
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestEqual(TEXT("compile clean"), Result.Data->GetStringField(TEXT("compile_status")), FString(TEXT("success")));
	}
	else if (!Result.bSuccess)
	{
		AddError(FString::Printf(TEXT("local rename failed: %s %s"), *Result.ErrorCode, *Result.ErrorMessage));
	}

	TestNotNull(TEXT("Local declaration renamed"), FBlueprintEditorUtils::FindLocalVariable(Blueprint, Calc, TEXT("Count2")));
	TestNull(TEXT("Old local declaration gone"), FBlueprintEditorUtils::FindLocalVariable(Blueprint, Calc, TEXT("Counter")));
	TestNotNull(TEXT("Other function keeps its local of the old name"), FBlueprintEditorUtils::FindLocalVariable(Blueprint, Mixed, TEXT("Counter")));
	for (const UK2Node_Variable* Node : {static_cast<const UK2Node_Variable*>(LocalGet), static_cast<const UK2Node_Variable*>(LocalSet)})
	{
		TestEqual(TEXT("Local node follows"), Node->GetVarName(), FName(TEXT("Count2")));
		TestTrue(TEXT("Local node stays local"), Node->VariableReference.IsLocalScope());
	}
	TestEqual(TEXT("Member node untouched"), MemberGet->GetVarName(), FName(TEXT("Counter")));
	TestTrue(TEXT("Member node still self member"), MemberGet->VariableReference.IsSelfContext() && !MemberGet->VariableReference.IsLocalScope());
	TestEqual(TEXT("Member node in the other function untouched"), MixedMemberGet->GetVarName(), FName(TEXT("Counter")));
	TestTrue(TEXT("Member variable untouched"), HasVariable(Blueprint, TEXT("Counter")));
	TestFalse(TEXT("No member variable was created"), HasVariable(Blueprint, TEXT("Count2")));
	if (Blueprint->GeneratedClass != nullptr)
	{
		const FBoolProperty* Property = FindFProperty<FBoolProperty>(Blueprint->GeneratedClass, TEXT("Counter"));
		if (TestNotNull(TEXT("Member property"), Property))
		{
			TestTrue(TEXT("Member default untouched"), Property->GetPropertyValue_InContainer(Blueprint->GeneratedClass->GetDefaultObject()));
		}
	}

	// Collisions inside the function.
	{
		TestTrue(TEXT("Second local added"), FBlueprintEditorUtils::AddLocalVariable(Blueprint, Calc, TEXT("Other"), BoolPin()));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		const FUnchanged Before(Blueprint);
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(LocalParams(TEXT("Calc"), TEXT("Count2"), TEXT("Other"))),
			CortexErrorCodes::NameCollision, TEXT("another local of the function"));
		Before.Check(*this, Blueprint, TEXT("local collision"));
	}

	ClearNodes(Blueprint);
	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameMemberLocalFaultInjectionTest,
	"Cortex.Blueprint.RenameMember.LocalFaultInjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameMemberLocalFaultInjectionTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using namespace CortexRenameTest;
	using namespace CortexRenameMemberFunctionTestLocal;
	UBlueprint* Blueprint = MakeBlueprint(TEXT("RenameLocalFault"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	UEdGraph* Quiet = AddFunctionGraph(Blueprint, TEXT("Quiet"));
	if (!TestNotNull(TEXT("Function graph"), Quiet))
	{
		Discard(Blueprint);
		return false;
	}
	// A local no node references: its declaration lives only on the entry node, outside the graph authoring hash.
	TestTrue(TEXT("Local added"), FBlueprintEditorUtils::AddLocalVariable(Blueprint, Quiet, TEXT("Idle"), BoolPin()));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const FUnchanged Before(Blueprint);

	FCortexCommandResult Result;
	{
		// After the rename the local's default is changed behind the command's back, so the readback fails; the editor undo is
		// skipped so that only the operation's own Capture/Restore can bring the declaration back.
		const FSkipUndoScope SkipUndo;
		const FPostApplyHookScope Hook([](UBlueprint* Target)
		{
			if (UK2Node_FunctionEntry* Entry = EntryOf(FunctionGraphNamed(Target, TEXT("Quiet"))))
			{
				for (FBPVariableDescription& Local : Entry->LocalVariables)
				{
					if (Local.VarName == FName(TEXT("Rested")))
					{
						Local.DefaultValue = TEXT("true");
					}
				}
			}
		});
		TSharedPtr<FJsonObject> Params = RenameParams(Blueprint, TEXT("local"), TEXT("Idle"), TEXT("Rested"));
		Params->SetStringField(TEXT("function"), TEXT("Quiet"));
		Result = FCortexBPRenameMemberOps::Execute(Params);
	}

	TestFalse(TEXT("The forced failure fails the call"), Result.bSuccess);
	TestEqual(TEXT("Readback mismatch"), Result.ErrorCode, CortexErrorCodes::ReadbackMismatch);
	if (TestTrue(TEXT("Failure carries details"), Result.ErrorDetails.IsValid()))
	{
		TestEqual(TEXT("Restored, not blocked"), Result.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
		TestFalse(TEXT("renamed is false"), Result.ErrorDetails->GetBoolField(TEXT("renamed")));
	}
	const FBPVariableDescription* Idle = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Quiet, TEXT("Idle"));
	TestNotNull(TEXT("Local declaration restored under the old name"), Idle);
	TestNull(TEXT("Renamed declaration gone"), FBlueprintEditorUtils::FindLocalVariable(Blueprint, Quiet, TEXT("Rested")));
	if (Idle != nullptr)
	{
		TestTrue(TEXT("Restored default is the original"), Idle->DefaultValue.IsEmpty());
	}
	TestTrue(TEXT("Blueprint up to date after the recovery compile"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	// A restored package may stay dirty (the failed attempt touched it); everything else must match.
	Before.Check(*this, Blueprint, TEXT("fault injection"), /*bCheckDirty=*/false);

	ClearNodes(Blueprint);
	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameMemberLocalChildCollisionTest,
	"Cortex.Blueprint.RenameMember.LocalChildCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameMemberLocalChildCollisionTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using namespace CortexRenameTest;

	UBlueprint* Owner = MakeBlueprint(TEXT("RenameLocalOwner"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Owner"), Owner))
	{
		return false;
	}
	UEdGraph* Calc = AddFunctionGraph(Owner, TEXT("Calc"));
	TestTrue(TEXT("Local added"), FBlueprintEditorUtils::AddLocalVariable(Owner, Calc, TEXT("Temp"), BoolPin()));
	FKismetEditorUtilities::CompileBlueprint(Owner);
	UBlueprint* Child = MakeBlueprint(TEXT("RenameLocalChild"), Owner->GeneratedClass);
	if (!TestNotNull(TEXT("Child"), Child))
	{
		Discard(Owner);
		return false;
	}
	AddVariable(Child, TEXT("Clash"), BoolPin(), TEXT("true"));
	FKismetEditorUtilities::CompileBlueprint(Child);

	const FUnchanged OwnerBefore(Owner);
	const FUnchanged ChildBefore(Child);
	TSharedPtr<FJsonObject> Params = RenameParams(Owner, TEXT("local"), TEXT("Temp"), TEXT("Clash"));
	Params->SetStringField(TEXT("function"), TEXT("Calc"));
	TSharedPtr<FJsonObject> Dry = RenameParams(Owner, TEXT("local"), TEXT("Temp"), TEXT("Clash"));
	Dry->SetStringField(TEXT("function"), TEXT("Calc"));
	Dry->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult DryResult = FCortexBPRenameMemberOps::Execute(Dry);
	if (TestTrue(TEXT("dry_run succeeds"), DryResult.bSuccess))
	{
		TestFalse(TEXT("dry_run reports the child collision"), DryResult.Data->GetBoolField(TEXT("can_apply")));
	}
	ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(Params), CortexErrorCodes::NameCollision, TEXT("a loaded child declares the new local name"));
	OwnerBefore.Check(*this, Owner, TEXT("owner"));
	ChildBefore.Check(*this, Child, TEXT("child is not renamed or dirtied"));
	TestTrue(TEXT("The child's variable keeps its name"), HasVariable(Child, TEXT("Clash")));

	ClearNodes(Child);
	ClearNodes(Owner);
	Discard(Child);
	Discard(Owner);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameMemberLocalExternalNodesTest,
	"Cortex.Blueprint.RenameMember.LocalExternalNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameMemberLocalExternalNodesTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using namespace CortexRenameTest;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("RenameLocalExternal"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	UEdGraph* Calc = AddFunctionGraph(Blueprint, TEXT("Calc"));
	TestTrue(TEXT("Local added"), FBlueprintEditorUtils::AddLocalVariable(Blueprint, Calc, TEXT("Counter"), BoolPin()));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UK2Node_VariableGet* LocalGet = AddGet(Calc, TEXT("Counter"), nullptr, TEXT("Calc"));
	// A node of the same name that reads a member of an unrelated class: the engine's local rename leaves it alone.
	UK2Node_VariableGet* External = AddGet(Calc, TEXT("bHidden"), AActor::StaticClass());
	External->VariableReference.SetExternalMember(TEXT("Counter"), AActor::StaticClass());

	TSharedPtr<FJsonObject> Params = RenameParams(Blueprint, TEXT("local"), TEXT("Counter"), TEXT("Count2"));
	Params->SetStringField(TEXT("function"), TEXT("Calc"));
	Params->SetBoolField(TEXT("compile"), false);
	const FCortexCommandResult Result = FCortexBPRenameMemberOps::Execute(Params);
	TestTrue(TEXT("a same-named node of an unrelated class does not block the rename"), Result.bSuccess);
	TestEqual(TEXT("Local node follows"), LocalGet->GetVarName(), FName(TEXT("Count2")));
	TestEqual(TEXT("External node untouched"), External->GetVarName(), FName(TEXT("Counter")));

	ClearNodes(Blueprint);
	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameMemberDispatcherNodesTest,
	"Cortex.Blueprint.RenameMember.DispatcherNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameMemberDispatcherNodesTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using namespace CortexRenameTest;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("RenameDispatcherNodes"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AddDispatcher(Blueprint, TEXT("OnDone"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);

	// A bind node that targets the Blueprint's own class from outside the self context, and the bound-event nodes a Level
	// Blueprint or a component binding would hold: none are retargeted by RenameMemberVariable.
	FGraphNodeCreator<UK2Node_AddDelegate> BindCreator(*EventGraph);
	UK2Node_AddDelegate* Bind = BindCreator.CreateNode();
	Bind->DelegateReference.SetExternalMember(TEXT("OnDone"), Blueprint->GeneratedClass);
	BindCreator.Finalize();
	UK2Node_ActorBoundEvent* ActorBound = NewObject<UK2Node_ActorBoundEvent>(EventGraph);
	ActorBound->CreateNewGuid();
	ActorBound->DelegateOwnerClass = Blueprint->GeneratedClass;
	ActorBound->DelegatePropertyName = TEXT("OnDone");
	EventGraph->AddNode(ActorBound, false, false);
	ActorBound->AllocateDefaultPins();
	UK2Node_ComponentBoundEvent* ComponentBound = NewObject<UK2Node_ComponentBoundEvent>(EventGraph);
	ComponentBound->CreateNewGuid();
	ComponentBound->DelegateOwnerClass = Blueprint->GeneratedClass;
	ComponentBound->DelegatePropertyName = TEXT("OnDone");
	EventGraph->AddNode(ComponentBound, false, false);
	ComponentBound->AllocateDefaultPins();
	UK2Node_ActorBoundEvent* Unrelated = NewObject<UK2Node_ActorBoundEvent>(EventGraph);
	Unrelated->CreateNewGuid();
	Unrelated->DelegateOwnerClass = AActor::StaticClass();
	Unrelated->DelegatePropertyName = TEXT("OnDone");
	EventGraph->AddNode(Unrelated, false, false);
	Unrelated->AllocateDefaultPins();

	TSharedPtr<FJsonObject> Params = RenameParams(Blueprint, TEXT("dispatcher"), TEXT("OnDone"), TEXT("OnFinished"));
	Params->SetBoolField(TEXT("compile"), false);
	const FCortexCommandResult Result = FCortexBPRenameMemberOps::Execute(Params);
	if (!TestTrue(TEXT("rename succeeds"), Result.bSuccess))
	{
		AddError(FString::Printf(TEXT("%s %s"), *Result.ErrorCode, *Result.ErrorMessage));
	}
	TestEqual(TEXT("Non-self bind node follows"), Bind->DelegateReference.GetMemberName(), FName(TEXT("OnFinished")));
	TestEqual(TEXT("Actor bound event follows"), ActorBound->DelegatePropertyName, FName(TEXT("OnFinished")));
	TestEqual(TEXT("Component bound event follows"), ComponentBound->DelegatePropertyName, FName(TEXT("OnFinished")));
	TestEqual(TEXT("A bound event of an unrelated class is not touched"), Unrelated->DelegatePropertyName, FName(TEXT("OnDone")));

	ClearNodes(Blueprint);
	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameMemberStaleAcknowledgmentTest,
	"Cortex.Blueprint.RenameMember.StaleAcknowledgment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameMemberStaleAcknowledgmentTest::RunTest(const FString& Parameters)
{
	using namespace CortexRenameMemberFunctionTestLocal;
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using namespace CortexRenameTest;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("RenameStaleAck"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AddFunctionGraph(Blueprint, TEXT("Calc"));
	AddDispatcher(Blueprint, TEXT("OnDone"));
	UEdGraph* Calc = FunctionGraphNamed(Blueprint, TEXT("Calc"));
	FBlueprintEditorUtils::AddLocalVariable(Blueprint, Calc, TEXT("Temp"), BoolPin());
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	const FUnchanged Before(Blueprint);
	const TSharedPtr<FJsonObject> Function = RenameParams(Blueprint, TEXT("function"), TEXT("Calc"), TEXT("Calc2"));
	const TSharedPtr<FJsonObject> Dispatcher = RenameParams(Blueprint, TEXT("dispatcher"), TEXT("OnDone"), TEXT("OnFinished"));
	const TSharedPtr<FJsonObject> Local = RenameParams(Blueprint, TEXT("local"), TEXT("Temp"), TEXT("Temp2"));
	Local->SetStringField(TEXT("function"), TEXT("Calc"));
	for (const TSharedPtr<FJsonObject>& Params : {Local, Dispatcher, Function})
	{
		SetStrings(Params, TEXT("acknowledged_losses"), {TEXT("save_game:Ghost")});
		ExpectRefusal(*this, FCortexBPRenameMemberOps::Execute(Params), CortexErrorCodes::PotentialDataLoss, Params->GetStringField(TEXT("kind")) + TEXT(" with a stale acknowledgment"));
		Before.Check(*this, Blueprint, Params->GetStringField(TEXT("kind")));
	}

	ClearNodes(Blueprint);
	Discard(Blueprint);
	return true;
}
