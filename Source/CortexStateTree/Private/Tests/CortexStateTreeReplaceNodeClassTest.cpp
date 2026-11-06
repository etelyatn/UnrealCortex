#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "CortexStateTreeMigrationFixture.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "StateTreeTypes.h"

#if !UE_VERSION_OLDER_THAN(5, 7, 0)
namespace
{
const TArray<TSharedPtr<FJsonValue>>& MigClassArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	static const TArray<TSharedPtr<FJsonValue>> Empty;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values != nullptr ? *Values : Empty;
}

TSharedPtr<FJsonObject> MigClassObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Child = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Field, Child) && Child != nullptr ? *Child : nullptr;
}

FString MigClassString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Field, Value);
	}
	return Value;
}

TSharedPtr<FJsonObject> MigClassReadNode(CortexSTTest::FMigrationFixture& Fixture, const FGuid& NodeId)
{
	TSharedPtr<FJsonObject> Read = Fixture.BaseParams();
	Read->SetStringField(TEXT("node_id"), NodeId.ToString(EGuidFormats::DigitsWithHyphens));
	const FCortexCommandResult Result = Fixture.Run(TEXT("get_node_bindings"), Read);
	return Result.bSuccess && MigClassArray(Result.Data, TEXT("nodes")).Num() == 1 ? MigClassArray(Result.Data, TEXT("nodes"))[0]->AsObject() : nullptr;
}

FString MigClassNodeStructName(const FStateTreeEditorNode& Node)
{
	return Node.Node.GetScriptStruct() != nullptr ? Node.Node.GetScriptStruct()->GetName() : FString();
}

TMap<FString, FString> MigClassIdentityMap()
{
	TMap<FString, FString> Map;
	Map.Add(TEXT("Count"), TEXT("Count"));
	Map.Add(TEXT("Label"), TEXT("Label"));
	Map.Add(TEXT("ResultOut"), TEXT("ResultOut"));
	return Map;
}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeReplaceNodeClassTest,
	"Cortex.StateTree.ReplaceNode.NativeBlueprintBase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeReplaceNodeClassTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	FCortexStateTreeCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = CortexStateTreeTest::Params();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/NotUsed"));
	TestEqual(TEXT("older engines report NOT_AVAILABLE"), Handler.Execute(TEXT("replace_node"), Params).ErrorCode, CortexErrorCodes::NotAvailable);
	return true;
#else
	CortexSTTest::FMigrationFixture Fixture;
	if (!Fixture.Build(*this, /*bCompile=*/true))
	{
		return false;
	}
	FStateTreeEditorNode* BpNode = Fixture.NodePtr(Fixture.BpTaskId);
	if (!TestNotNull(TEXT("the Blueprint node exists"), BpNode)
		|| !TestTrue(TEXT("Label is set to a non-default value"), CortexSTTest::SetInstanceValueText(*BpNode, TEXT("Label"), TEXT("x")))
		|| !TestTrue(TEXT("Count is set to a non-default value"), CortexSTTest::SetInstanceValueText(*BpNode, TEXT("Count"), TEXT("7"))))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Params = Fixture.MutationParams();
	Params->SetStringField(TEXT("node_id"), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
	Params->SetStringField(TEXT("new_node_class"), UCortexSTTestNativeBlueprintTask::StaticClass()->GetPathName());
	TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Entry : MigClassIdentityMap())
	{
		Map->SetStringField(Entry.Key, Entry.Value);
	}
	Params->SetObjectField(TEXT("property_map"), Map);
	const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), Params);
	if (!TestTrue(TEXT("the class replacement succeeds"), Result.bSuccess))
	{
		AddError(Result.ErrorMessage);
		return false;
	}

	TestEqual(TEXT("the class replacement compiles"), MigClassString(Result.Data, TEXT("compile_status")), FString(TEXT("success")));
	TestEqual(TEXT("the class replacement keeps both bindings"), static_cast<int32>(MigClassObject(Result.Data, TEXT("bindings"))->GetNumberField(TEXT("after_count"))), 2);
	TestTrue(TEXT("the class replacement is ready to run"), Fixture.Context.StateTree->IsReadyToRun());

	FStateTreeEditorNode* Replaced = Fixture.NodePtr(Fixture.BpTaskId);
	if (!TestNotNull(TEXT("the node keeps its ID"), Replaced))
	{
		return false;
	}
	TestEqual(TEXT("the wrapper node struct is kept"), MigClassNodeStructName(*Replaced), FString(TEXT("StateTreeBlueprintTaskWrapper")));
	const FStateTreeBlueprintTaskWrapper& Wrapper = Replaced->Node.Get<FStateTreeBlueprintTaskWrapper>();
	TestEqual(TEXT("the wrapper points at the native class"), Wrapper.TaskClass.Get(), UCortexSTTestNativeBlueprintTask::StaticClass());
	if (TestNotNull(TEXT("an instance object of the native class exists"), Replaced->InstanceObject.Get()))
	{
		TestEqual(TEXT("the instance object is the native class"), Replaced->InstanceObject->GetClass(), UCortexSTTestNativeBlueprintTask::StaticClass());
		const UCortexSTTestNativeBlueprintTask* NativeInstance = Cast<UCortexSTTestNativeBlueprintTask>(Replaced->InstanceObject.Get());
		if (TestNotNull(TEXT("the native instance casts"), const_cast<UCortexSTTestNativeBlueprintTask*>(NativeInstance)))
		{
			TestEqual(TEXT("Count is copied by name"), NativeInstance->Count, 7);
			TestEqual(TEXT("Label is copied by name"), NativeInstance->Label, FString(TEXT("x")));
		}
	}
	TestEqual(TEXT("the node keeps its name"), Replaced->GetName().ToString(), FString(TEXT("BP_T")));

	const TSharedPtr<FJsonObject> Node = MigClassReadNode(Fixture, Fixture.BpTaskId);
	if (TestNotNull(TEXT("the replaced node reads"), Node.Get()))
	{
		const TSharedPtr<FJsonObject> NodeType = MigClassObject(Node, TEXT("node_type"));
		TestEqual(TEXT("the node type kind is a Blueprint-base class"), MigClassString(NodeType, TEXT("kind")), FString(TEXT("blueprint_class")));
		TestFalse(TEXT("a native Blueprint-base class is not a Blueprint"), NodeType->GetBoolField(TEXT("is_blueprint")));
		TestEqual(TEXT("the class path is the native class"), MigClassString(NodeType, TEXT("class_path")), UCortexSTTestNativeBlueprintTask::StaticClass()->GetPathName());
		bool bAllResolved = true;
		for (const TCHAR* Side : { TEXT("as_target"), TEXT("as_source") })
		{
			for (const TSharedPtr<FJsonValue>& Value : MigClassArray(Node, Side))
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				bAllResolved = bAllResolved
					&& MigClassObject(Entry, TEXT("source"))->GetBoolField(TEXT("resolved"))
					&& MigClassObject(Entry, TEXT("target"))->GetBoolField(TEXT("resolved"));
			}
		}
		TestTrue(TEXT("every binding of the node still resolves"), bAllResolved);
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeReplaceNodeConditionTest,
	"Cortex.StateTree.ReplaceNode.Condition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeReplaceNodeConditionTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	return true;
#else
	CortexSTTest::FMigrationFixture Fixture;
	if (!Fixture.Build(*this, /*bCompile=*/true))
	{
		return false;
	}
	UStateTreeState* A = Fixture.StateA();
	UStateTreeState* B = Fixture.StateB();
	if (!TestNotNull(TEXT("state A exists"), A) || !TestNotNull(TEXT("state B exists"), B))
	{
		return false;
	}

	// A Blueprint condition class with one bool parameter, used in an enter condition and in a transition condition.
	Fixture.ExtraAssets.Add(Fixture.Folder / TEXT("BP_C"));
	UBlueprint* ConditionBlueprint = CortexSTTest::MakeTaskBlueprint(
		Fixture.Folder,
		TEXT("BP_C"),
		UStateTreeConditionBlueprintBase::StaticClass(),
		{ { FName(TEXT("bResult")), UEdGraphSchema_K2::PC_Boolean, FString(), FString(), true, nullptr } });
	if (!TestNotNull(TEXT("the Blueprint condition class is created"), ConditionBlueprint)
		|| !TestNotNull(TEXT("the Blueprint condition has a generated class"), ConditionBlueprint->GeneratedClass.Get()))
	{
		return false;
	}
	FStateTreeBlueprintConditionWrapper Wrapper;
	Wrapper.ConditionClass = ConditionBlueprint->GeneratedClass.Get();

	// Conditions cannot take task instance data as their source, so the source is a global evaluator's bool.
	FStateTreeEditorNode& Evaluator = Fixture.Context.EditorData->AddEvaluator<FCortexSTTestNativeEvaluator>();

	FStateTreeEditorNode& EnterCondition = A->AddEnterCondition<FStateTreeBlueprintConditionWrapper>(Wrapper);
	EnterCondition.ExpressionIndent = 2;
	EnterCondition.ExpressionOperand = EStateTreeExpressionOperand::Or;
	const FGuid EnterConditionId = EnterCondition.ID;

	FStateTreeTransition& Transition = CortexSTCompat::AddTransition(*A, EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, B);
	FStateTreeEditorNode& TransitionCondition = Transition.AddConditionWithOuter<FStateTreeBlueprintConditionWrapper>(A, Wrapper);
	TransitionCondition.ExpressionIndent = 1;
	TransitionCondition.ExpressionOperand = EStateTreeExpressionOperand::Or;
	const FGuid TransitionConditionId = TransitionCondition.ID;

	Fixture.Context.EditorData->AddPropertyBinding(
		FPropertyBindingPath(Evaluator.ID, FName(TEXT("bResult"))),
		FPropertyBindingPath(EnterConditionId, FName(TEXT("bResult"))));
	Fixture.Context.EditorData->AddPropertyBinding(
		FPropertyBindingPath(Evaluator.ID, FName(TEXT("bResult"))),
		FPropertyBindingPath(TransitionConditionId, FName(TEXT("bResult"))));

	const FCortexCommandResult Compiled = Fixture.Compile();
	if (!TestTrue(TEXT("the fixture with two Blueprint conditions compiles"), Compiled.bSuccess))
	{
		AddError(Compiled.ErrorMessage);
		return false;
	}

	TSharedPtr<FJsonObject> Before = Fixture.BaseParams();
	Before->SetStringField(TEXT("node_id"), EnterConditionId.ToString(EGuidFormats::DigitsWithHyphens));
	const FCortexCommandResult BeforeRead = Fixture.Run(TEXT("get_node_bindings"), Before);
	TestEqual(TEXT("the whole tree carries four bindings"), static_cast<int32>(BeforeRead.Data->GetNumberField(TEXT("whole_tree_binding_count"))), 4);

	for (const TPair<FGuid, int32>& Target : { TPair<FGuid, int32>(EnterConditionId, 2), TPair<FGuid, int32>(TransitionConditionId, 1) })
	{
		TSharedPtr<FJsonObject> Params = Fixture.MutationParams();
		Params->SetStringField(TEXT("node_id"), Target.Key.ToString(EGuidFormats::DigitsWithHyphens));
		Params->SetStringField(TEXT("new_node_struct"), FCortexSTTestNativeCondition::StaticStruct()->GetPathName());
		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), Params);
		if (!TestTrue(TEXT("the condition replacement succeeds"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		TestEqual(TEXT("the condition tree compiles"), MigClassString(Result.Data, TEXT("compile_status")), FString(TEXT("success")));
		TestEqual(TEXT("the condition binding count is unchanged"), static_cast<int32>(MigClassObject(Result.Data, TEXT("bindings"))->GetNumberField(TEXT("after_count"))), 4);

		FStateTreeEditorNode* Condition = Fixture.NodePtr(Target.Key);
		if (!TestNotNull(TEXT("the condition node keeps its ID"), Condition))
		{
			return false;
		}
		TestEqual(TEXT("the condition is the native condition struct"), MigClassNodeStructName(*Condition), FCortexSTTestNativeCondition::StaticStruct()->GetName());
		TestEqual(TEXT("ExpressionIndent is preserved"), static_cast<int32>(Condition->ExpressionIndent), Target.Value);
		TestTrue(TEXT("ExpressionOperand is preserved"), Condition->ExpressionOperand == EStateTreeExpressionOperand::Or);

		const TSharedPtr<FJsonObject> Node = MigClassReadNode(Fixture, Target.Key);
		if (TestNotNull(TEXT("the condition node reads"), Node.Get()))
		{
			TestFalse(TEXT("the condition is no longer a Blueprint"), MigClassObject(Node, TEXT("node_type"))->GetBoolField(TEXT("is_blueprint")));
			const TArray<TSharedPtr<FJsonValue>>& Targets = MigClassArray(Node, TEXT("as_target"));
			if (TestEqual(TEXT("the condition keeps its one binding"), Targets.Num(), 1))
			{
				const TSharedPtr<FJsonObject> Entry = Targets[0]->AsObject();
				TestTrue(TEXT("the condition binding still resolves"), MigClassObject(Entry, TEXT("target"))->GetBoolField(TEXT("resolved")));
				TestEqual(TEXT("the condition binding still targets bResult"), MigClassString(MigClassObject(Entry, TEXT("target")), TEXT("path")), FString(TEXT("bResult")));
			}
		}
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeReplaceNodeBulkTest,
	"Cortex.StateTree.ReplaceNode.BlueprintClassBulk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeReplaceNodeBulkTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	return true;
#else
	CortexSTTest::FMigrationFixture Fixture;
	if (!Fixture.Build(*this, /*bCompile=*/true))
	{
		return false;
	}
	UStateTreeState* A = Fixture.StateA();
	UStateTreeState* B = Fixture.StateB();
	if (!TestNotNull(TEXT("state A exists"), A) || !TestNotNull(TEXT("state B exists"), B))
	{
		return false;
	}

	// The same Blueprint task class on three nodes in two states: BP_T in A (from the fixture), one more in A and one in B.
	FStateTreeBlueprintTaskWrapper Wrapper;
	Wrapper.TaskClass = Fixture.TaskBlueprint->GeneratedClass.Get();
	const FGuid SecondInA = A->AddTask<FStateTreeBlueprintTaskWrapper>(Wrapper).ID;
	const FGuid OnlyInB = B->AddTask<FStateTreeBlueprintTaskWrapper>(Wrapper).ID;
	Fixture.Context.StateTree->MarkPackageDirty();
	if (!TestTrue(TEXT("the three-node fixture compiles"), Fixture.Compile().bSuccess))
	{
		return false;
	}

	const int32 UndoBefore = GEditor != nullptr && GEditor->Trans != nullptr ? GEditor->Trans->GetQueueLength() : 0;
	TSharedPtr<FJsonObject> Params = Fixture.MutationParams();
	Params->SetStringField(TEXT("blueprint_class"), Fixture.TaskBlueprint->GeneratedClass->GetPathName());
	Params->SetStringField(TEXT("new_node_struct"), FCortexSTTestNativeTask::StaticStruct()->GetPathName());
	TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
	Map->SetStringField(TEXT("Count"), TEXT("Count"));
	Map->SetStringField(TEXT("ResultOut"), TEXT("ResultOut"));
	Params->SetObjectField(TEXT("property_map"), Map);
	const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), Params);
	if (!TestTrue(TEXT("the bulk replacement succeeds"), Result.bSuccess))
	{
		AddError(Result.ErrorMessage);
		return false;
	}

	TestEqual(TEXT("all three nodes are replaced"), MigClassArray(Result.Data, TEXT("replaced")).Num(), 3);
	TestEqual(TEXT("the bulk replacement compiles once with no error"), static_cast<int32>(Result.Data->GetNumberField(TEXT("error_count"))), 0);
	TestEqual(TEXT("the bulk replacement keeps both bindings"), static_cast<int32>(MigClassObject(Result.Data, TEXT("bindings"))->GetNumberField(TEXT("after_count"))), 2);
	TestEqual(TEXT("the bulk replacement opens exactly one transaction"), GEditor->Trans->GetQueueLength(), UndoBefore + 1);
	TestTrue(TEXT("the bulk replacement is ready to run"), Fixture.Context.StateTree->IsReadyToRun());
	for (const FGuid& NodeId : { Fixture.BpTaskId, SecondInA, OnlyInB })
	{
		FStateTreeEditorNode* Node = Fixture.NodePtr(NodeId);
		TestEqual(TEXT("every node of the class is native now"),
			Node != nullptr ? Node->Node.GetScriptStruct()->GetName() : FString(),
			FCortexSTTestNativeTask::StaticStruct()->GetName());
	}

	TestTrue(TEXT("one undo succeeds"), GEditor->UndoTransaction());
	int32 WrapperNodes = 0;
	TArray<FCortexSTNodeLocation> AllNodes;
	CortexST::CollectEditorNodes(Fixture.Context, AllNodes);
	for (const FCortexSTNodeLocation& Location : AllNodes)
	{
		if (Location.Node->Node.GetScriptStruct() == FStateTreeBlueprintTaskWrapper::StaticStruct())
		{
			++WrapperNodes;
			TestEqual(TEXT("the restored wrapper points at the Blueprint class"),
				Location.Node->InstanceObject != nullptr ? Location.Node->InstanceObject->GetClass() : nullptr,
				Fixture.TaskBlueprint->GeneratedClass.Get());
		}
	}
	TestEqual(TEXT("the single undo restores all three Blueprint nodes"), WrapperNodes, 3);
	TestEqual(TEXT("the single undo restores every binding"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), 2);

	// A class this tree does not use: nothing to replace.
	TSharedPtr<FJsonObject> Empty = Fixture.MutationParams();
	Empty->SetStringField(TEXT("blueprint_class"), UCortexSTTestNativeBlueprintTask::StaticClass()->GetPathName());
	Empty->SetStringField(TEXT("new_node_struct"), FCortexSTTestNativeTask::StaticStruct()->GetPathName());
	const FCortexCommandResult Missing = Fixture.Run(TEXT("replace_node"), Empty);
	TestFalse(TEXT("a class with no nodes is refused"), Missing.bSuccess);
	TestEqual(TEXT("a class with no nodes code"), Missing.ErrorCode, CortexErrorCodes::NodeNotFound);
	return true;
#endif
}
