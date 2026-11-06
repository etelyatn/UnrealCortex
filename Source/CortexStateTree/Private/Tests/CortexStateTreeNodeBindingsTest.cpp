#include "Misc/AutomationTest.h"
#include "CortexSTCompat.h"
#include "Misc/EngineVersionComparison.h"
#include "CortexStateTreeMigrationFixture.h"
#include "Dom/JsonValue.h"

#if CORTEXST_HAS_NODE_BINDINGS
namespace
{
const TArray<TSharedPtr<FJsonValue>>& MigNBArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	static const TArray<TSharedPtr<FJsonValue>> Empty;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values != nullptr ? *Values : Empty;
}

TSharedPtr<FJsonObject> MigNBObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Child = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Field, Child) && Child != nullptr ? *Child : nullptr;
}

FString MigNBString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Field, Value);
	}
	return Value;
}

TSharedPtr<FJsonObject> MigNBFindNode(const TSharedPtr<FJsonObject>& Data, const FGuid& NodeId)
{
	const FString Wanted = NodeId.ToString(EGuidFormats::DigitsWithHyphens);
	for (const TSharedPtr<FJsonValue>& Value : MigNBArray(Data, TEXT("nodes")))
	{
		const TSharedPtr<FJsonObject> Node = Value->AsObject();
		if (MigNBString(Node, TEXT("node_id")) == Wanted)
		{
			return Node;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> MigNBFirstSegment(const TSharedPtr<FJsonObject>& Side)
{
	const TArray<TSharedPtr<FJsonValue>>& Segments = MigNBArray(Side, TEXT("segments"));
	return Segments.Num() > 0 ? Segments[0]->AsObject() : nullptr;
}

bool MigNBGuidIsNull(const TSharedPtr<FJsonObject>& Segment)
{
	return Segment.IsValid() && Segment->HasField(TEXT("property_guid")) && Segment->GetField<EJson::None>(TEXT("property_guid"))->IsNull();
}

bool MigNBFingerprintEquals(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
{
	return A.IsValid() && B.IsValid()
		&& FJsonValue::CompareEqual(*MakeShared<FJsonValueObject>(A), *MakeShared<FJsonValueObject>(B));
}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeNodeBindingsTest,
	"Cortex.StateTree.NodeBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeNodeBindingsTest::RunTest(const FString& Parameters)
{
#if !CORTEXST_HAS_NODE_BINDINGS
	FCortexStateTreeCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = CortexStateTreeTest::Params();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/NotUsed"));
	const FCortexCommandResult Result = Handler.Execute(TEXT("get_node_bindings"), Params);
	TestFalse(TEXT("older engines refuse"), Result.bSuccess);
	TestEqual(TEXT("older engines report NOT_AVAILABLE"), Result.ErrorCode, CortexErrorCodes::NotAvailable);
	return true;
#else
	CortexSTTest::FMigrationFixture Fixture;
	if (!Fixture.Build(*this, /*bCompile=*/true))
	{
		return false;
	}

	// A task-completion binding: NativeA completes -> the delegate listener of a transition (source struct ID is the node-struct ID).
	UStateTreeState* A = Fixture.StateA();
	UStateTreeState* B = Fixture.StateB();
	if (!TestNotNull(TEXT("state B exists"), B))
	{
		return false;
	}
	FStateTreeTransition& Transition = CortexSTCompat::AddTransition(*A, EStateTreeTransitionTrigger::OnDelegate, EStateTreeTransitionType::GotoState, B);
	Fixture.Context.EditorData->EditorBindings.AddTaskCompletionBinding(
		Fixture.NodePtr(Fixture.NativeAId)->GetNodeID(),
		FPropertyBindingPath(Transition.ID, FName(TEXT("DelegateListener"))),
		UE::StateTree::ETaskCompletionCondition::Succeeds);

	UPackage* Package = Fixture.Context.StateTree->GetOutermost();
	Package->SetDirtyFlag(false);
	const TSharedPtr<FJsonObject> FingerprintBefore = CortexST::MakeFingerprint(Fixture.Context.StateTree);

	// ── All nodes with a binding, default paging ──
	TSharedPtr<FJsonObject> Params = Fixture.BaseParams();
	const FCortexCommandResult All = Fixture.Run(TEXT("get_node_bindings"), Params);
	if (!TestTrue(TEXT("get_node_bindings succeeds"), All.bSuccess))
	{
		AddError(All.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("three nodes carry bindings"), static_cast<int32>(All.Data->GetNumberField(TEXT("total"))), 3);
	TestEqual(TEXT("whole-tree binding count includes the completion binding"), static_cast<int32>(All.Data->GetNumberField(TEXT("whole_tree_binding_count"))), 3);

	const TSharedPtr<FJsonObject> NativeA = MigNBFindNode(All.Data, Fixture.NativeAId);
	const TSharedPtr<FJsonObject> BpTask = MigNBFindNode(All.Data, Fixture.BpTaskId);
	const TSharedPtr<FJsonObject> NativeB = MigNBFindNode(All.Data, Fixture.NativeBId);
	if (!TestTrue(TEXT("all three nodes are reported"), NativeA.IsValid() && BpTask.IsValid() && NativeB.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("NativeA slot"), MigNBString(NativeA, TEXT("slot")), FString(TEXT("task")));
	TestEqual(TEXT("NativeA owner state path"), MigNBString(NativeA, TEXT("owner_state_path")), FString(TEXT("A")));
	TestEqual(TEXT("NativeB owner state path"), MigNBString(NativeB, TEXT("owner_state_path")), FString(TEXT("A/B")));
	TestEqual(TEXT("BP_T slot"), MigNBString(BpTask, TEXT("slot")), FString(TEXT("task")));
	TestEqual(TEXT("BP_T name"), MigNBString(BpTask, TEXT("name")), FString(TEXT("BP_T")));
	TestFalse(TEXT("definition_id is reported"), MigNBString(BpTask, TEXT("definition_id")).IsEmpty());
	TestTrue(TEXT("BP_T node_type is a Blueprint class"), MigNBObject(BpTask, TEXT("node_type"))->GetBoolField(TEXT("is_blueprint")));
	TestEqual(TEXT("BP_T node_type kind"), MigNBString(MigNBObject(BpTask, TEXT("node_type")), TEXT("kind")), FString(TEXT("blueprint_class")));
	TestFalse(TEXT("NativeA node_type is not a Blueprint"), MigNBObject(NativeA, TEXT("node_type"))->GetBoolField(TEXT("is_blueprint")));
	TestEqual(TEXT("NativeA node_type kind"), MigNBString(MigNBObject(NativeA, TEXT("node_type")), TEXT("kind")), FString(TEXT("struct")));
	TestEqual(TEXT("NativeA struct path"), MigNBString(MigNBObject(NativeA, TEXT("node_type")), TEXT("struct_path")), FCortexSTTestNativeTask::StaticStruct()->GetPathName());

	// ── per-node as_target / as_source ──
	TestEqual(TEXT("NativeA is no binding target"), MigNBArray(NativeA, TEXT("as_target")).Num(), 0);
	TestEqual(TEXT("NativeA is the source of the value binding and of the completion binding"), MigNBArray(NativeA, TEXT("as_source")).Num(), 2);
	TestEqual(TEXT("BP_T is the target of one binding"), MigNBArray(BpTask, TEXT("as_target")).Num(), 1);
	TestEqual(TEXT("BP_T is the source of one binding"), MigNBArray(BpTask, TEXT("as_source")).Num(), 1);
	TestEqual(TEXT("NativeB is the target of one binding"), MigNBArray(NativeB, TEXT("as_target")).Num(), 1);
	TestEqual(TEXT("NativeB is no binding source"), MigNBArray(NativeB, TEXT("as_source")).Num(), 0);

	// A.ResultOut -> BP_T.Count: native source segment has no property GUID, the Blueprint target segment has one.
	const TSharedPtr<FJsonObject> IntoBp = MigNBArray(BpTask, TEXT("as_target"))[0]->AsObject();
	TestEqual(TEXT("binding into BP_T has index 0"), static_cast<int32>(IntoBp->GetNumberField(TEXT("binding_index"))), 0);
	TestEqual(TEXT("binding into BP_T is an input binding"), MigNBString(IntoBp, TEXT("direction")), FString(TEXT("input")));
	TestFalse(TEXT("binding into BP_T is no function binding"), IntoBp->GetBoolField(TEXT("is_function_binding")));
	TestFalse(TEXT("binding into BP_T is no implicit context binding"), IntoBp->GetBoolField(TEXT("implicit_context")));
	const TSharedPtr<FJsonObject> IntoBpSource = MigNBObject(IntoBp, TEXT("source"));
	const TSharedPtr<FJsonObject> IntoBpTarget = MigNBObject(IntoBp, TEXT("target"));
	TestTrue(TEXT("native source path resolves"), IntoBpSource->GetBoolField(TEXT("resolved")));
	TestTrue(TEXT("Blueprint target path resolves"), IntoBpTarget->GetBoolField(TEXT("resolved")));
	TestEqual(TEXT("source path text"), MigNBString(IntoBpSource, TEXT("path")), FString(TEXT("ResultOut")));
	TestEqual(TEXT("target path text"), MigNBString(IntoBpTarget, TEXT("path")), FString(TEXT("Count")));
	TestEqual(TEXT("source owner is a node"), MigNBString(MigNBObject(IntoBpSource, TEXT("owner")), TEXT("kind")), FString(TEXT("node")));
	TestEqual(TEXT("source owner is NativeA"), MigNBString(MigNBObject(IntoBpSource, TEXT("owner")), TEXT("node_id")), Fixture.NativeAId.ToString(EGuidFormats::DigitsWithHyphens));
	TestEqual(TEXT("target owner is BP_T"), MigNBString(MigNBObject(IntoBpTarget, TEXT("owner")), TEXT("node_id")), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
	TestTrue(TEXT("native segment has a null property GUID"), MigNBGuidIsNull(MigNBFirstSegment(IntoBpSource)));
	TestFalse(TEXT("Blueprint-class segment has a property GUID"), MigNBGuidIsNull(MigNBFirstSegment(IntoBpTarget)));
	TestFalse(TEXT("Blueprint-class segment GUID text is not empty"), MigNBString(MigNBFirstSegment(IntoBpTarget), TEXT("property_guid")).IsEmpty());

	// BP_T.ResultOut -> NativeB.Count: the other way round.
	const TSharedPtr<FJsonObject> OutOfBp = MigNBArray(BpTask, TEXT("as_source"))[0]->AsObject();
	TestEqual(TEXT("binding out of BP_T has index 1"), static_cast<int32>(OutOfBp->GetNumberField(TEXT("binding_index"))), 1);
	TestFalse(TEXT("Blueprint source segment has a property GUID"), MigNBGuidIsNull(MigNBFirstSegment(MigNBObject(OutOfBp, TEXT("source")))));
	TestTrue(TEXT("native target segment has a null property GUID"), MigNBGuidIsNull(MigNBFirstSegment(MigNBObject(OutOfBp, TEXT("target")))));
	TestTrue(TEXT("both sides resolve"), MigNBObject(OutOfBp, TEXT("source"))->GetBoolField(TEXT("resolved")) && MigNBObject(OutOfBp, TEXT("target"))->GetBoolField(TEXT("resolved")));
	TestEqual(TEXT("target owner is NativeB"), MigNBString(MigNBObject(MigNBObject(OutOfBp, TEXT("target")), TEXT("owner")), TEXT("node_id")), Fixture.NativeBId.ToString(EGuidFormats::DigitsWithHyphens));

	// The task-completion binding has no source segments and names its condition.
	TSharedPtr<FJsonObject> Completion;
	for (const TSharedPtr<FJsonValue>& Value : MigNBArray(NativeA, TEXT("as_source")))
	{
		const TSharedPtr<FJsonObject> Entry = Value->AsObject();
		if (Entry->HasField(TEXT("completion_condition")))
		{
			Completion = Entry;
		}
	}
	if (TestTrue(TEXT("the completion binding is listed on its source task"), Completion.IsValid()))
	{
		TestEqual(TEXT("completion condition"), MigNBString(Completion, TEXT("completion_condition")), FString(TEXT("Succeeds")));
		TestEqual(TEXT("completion binding has no source segments"), MigNBArray(MigNBObject(Completion, TEXT("source")), TEXT("segments")).Num(), 0);
		TestTrue(TEXT("completion source resolves"), MigNBObject(Completion, TEXT("source"))->GetBoolField(TEXT("resolved")));
		TestEqual(TEXT("completion source is the task"), MigNBString(MigNBObject(MigNBObject(Completion, TEXT("source")), TEXT("owner")), TEXT("node_id")), Fixture.NativeAId.ToString(EGuidFormats::DigitsWithHyphens));
		TestEqual(TEXT("completion target is the transition listener"), MigNBString(MigNBObject(Completion, TEXT("target")), TEXT("path")), FString(TEXT("DelegateListener")));
		TestEqual(TEXT("completion binding index"), static_cast<int32>(Completion->GetNumberField(TEXT("binding_index"))), 2);
	}

	// ── selectors ──
	{
		TSharedPtr<FJsonObject> ByNode = Fixture.BaseParams();
		ByNode->SetStringField(TEXT("node_id"), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
		const FCortexCommandResult Result = Fixture.Run(TEXT("get_node_bindings"), ByNode);
		TestTrue(TEXT("node_id selector succeeds"), Result.bSuccess);
		if (Result.bSuccess)
		{
			TestEqual(TEXT("node_id selects exactly one node"), MigNBArray(Result.Data, TEXT("nodes")).Num(), 1);
			TestTrue(TEXT("the selected node is BP_T"), MigNBFindNode(Result.Data, Fixture.BpTaskId).IsValid());
		}
	}
	{
		TSharedPtr<FJsonObject> ByState = Fixture.BaseParams();
		ByState->SetStringField(TEXT("state_path"), TEXT("A"));
		const FCortexCommandResult Result = Fixture.Run(TEXT("get_node_bindings"), ByState);
		TestTrue(TEXT("state_path selector succeeds"), Result.bSuccess);
		if (Result.bSuccess)
		{
			TestEqual(TEXT("state A owns two nodes"), MigNBArray(Result.Data, TEXT("nodes")).Num(), 2);
			TestFalse(TEXT("NativeB is not owned by state A"), MigNBFindNode(Result.Data, Fixture.NativeBId).IsValid());
		}
		ByState->RemoveField(TEXT("state_path"));
		ByState->SetStringField(TEXT("state_id"), Fixture.StateBId.ToString(EGuidFormats::DigitsWithHyphens));
		const FCortexCommandResult ById = Fixture.Run(TEXT("get_node_bindings"), ByState);
		TestTrue(TEXT("state_id selector succeeds"), ById.bSuccess);
		if (ById.bSuccess)
		{
			TestEqual(TEXT("state B owns one node"), MigNBArray(ById.Data, TEXT("nodes")).Num(), 1);
		}
	}
	{
		TSharedPtr<FJsonObject> Both = Fixture.BaseParams();
		Both->SetStringField(TEXT("node_id"), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
		Both->SetStringField(TEXT("state_path"), TEXT("A"));
		const FCortexCommandResult Result = Fixture.Run(TEXT("get_node_bindings"), Both);
		TestFalse(TEXT("node_id and a state selector together are refused"), Result.bSuccess);
		TestEqual(TEXT("node_id plus state selector code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// ── paging ──
	{
		TSharedPtr<FJsonObject> Page = Fixture.BaseParams();
		Page->SetNumberField(TEXT("limit"), 1);
		const FCortexCommandResult First = Fixture.Run(TEXT("get_node_bindings"), Page);
		TestTrue(TEXT("first page succeeds"), First.bSuccess);
		if (First.bSuccess)
		{
			TestEqual(TEXT("first page returns one node"), MigNBArray(First.Data, TEXT("nodes")).Num(), 1);
			TestEqual(TEXT("first page total"), static_cast<int32>(First.Data->GetNumberField(TEXT("total"))), 3);
			TestTrue(TEXT("first page has more"), First.Data->GetBoolField(TEXT("has_more")));
		}
		Page->SetNumberField(TEXT("offset"), 2);
		const FCortexCommandResult Last = Fixture.Run(TEXT("get_node_bindings"), Page);
		if (TestTrue(TEXT("last page succeeds"), Last.bSuccess))
		{
			TestEqual(TEXT("last page returns one node"), MigNBArray(Last.Data, TEXT("nodes")).Num(), 1);
			TestFalse(TEXT("last page has no more"), Last.Data->GetBoolField(TEXT("has_more")));
		}
		Page->SetNumberField(TEXT("offset"), 3);
		const FCortexCommandResult Past = Fixture.Run(TEXT("get_node_bindings"), Page);
		if (TestTrue(TEXT("offset equal to total succeeds"), Past.bSuccess))
		{
			TestEqual(TEXT("offset equal to total is an empty page"), MigNBArray(Past.Data, TEXT("nodes")).Num(), 0);
		}
	}

	// ── direction ──
	{
		TSharedPtr<FJsonObject> Targets = Fixture.BaseParams();
		Targets->SetStringField(TEXT("direction"), TEXT("target"));
		const FCortexCommandResult Result = Fixture.Run(TEXT("get_node_bindings"), Targets);
		if (TestTrue(TEXT("direction=target succeeds"), Result.bSuccess))
		{
			TestEqual(TEXT("two nodes are binding targets"), static_cast<int32>(Result.Data->GetNumberField(TEXT("total"))), 2);
			const TSharedPtr<FJsonObject> Bp = MigNBFindNode(Result.Data, Fixture.BpTaskId);
			TestEqual(TEXT("direction=target drops as_source"), MigNBArray(Bp, TEXT("as_source")).Num(), 0);
		}
		Targets->SetStringField(TEXT("direction"), TEXT("source"));
		const FCortexCommandResult Sources = Fixture.Run(TEXT("get_node_bindings"), Targets);
		if (TestTrue(TEXT("direction=source succeeds"), Sources.bSuccess))
		{
			TestEqual(TEXT("two nodes are binding sources"), static_cast<int32>(Sources.Data->GetNumberField(TEXT("total"))), 2);
		}
		Targets->SetStringField(TEXT("direction"), TEXT("sideways"));
		TestEqual(TEXT("unknown direction is refused"), Fixture.Run(TEXT("get_node_bindings"), Targets).ErrorCode, CortexErrorCodes::InvalidField);
	}

	// ── refusals ──
	{
		TSharedPtr<FJsonObject> TooMany = Fixture.BaseParams();
		TooMany->SetNumberField(TEXT("limit"), 201);
		const FCortexCommandResult Result = Fixture.Run(TEXT("get_node_bindings"), TooMany);
		TestFalse(TEXT("limit above 200 is refused"), Result.bSuccess);
		TestEqual(TEXT("limit above 200 code"), Result.ErrorCode, CortexErrorCodes::LimitExceeded);

		TSharedPtr<FJsonObject> Zero = Fixture.BaseParams();
		Zero->SetNumberField(TEXT("limit"), 0);
		TestEqual(TEXT("limit 0 is refused"), Fixture.Run(TEXT("get_node_bindings"), Zero).ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> Unknown = Fixture.BaseParams();
		Unknown->SetStringField(TEXT("node_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
		const FCortexCommandResult Missing = Fixture.Run(TEXT("get_node_bindings"), Unknown);
		TestFalse(TEXT("unknown node_id is refused"), Missing.bSuccess);
		TestEqual(TEXT("unknown node_id code"), Missing.ErrorCode, CortexErrorCodes::NodeNotFound);

		TSharedPtr<FJsonObject> Malformed = Fixture.BaseParams();
		Malformed->SetStringField(TEXT("node_id"), TEXT("not-a-guid"));
		TestEqual(TEXT("malformed node_id is refused"), Fixture.Run(TEXT("get_node_bindings"), Malformed).ErrorCode, CortexErrorCodes::InvalidField);
	}

	// ── passive: no validation, no compile, no dirtying ──
	TestFalse(TEXT("reading bindings does not dirty the package"), Package->IsDirty());
	TestTrue(TEXT("reading bindings leaves the fingerprint unchanged"), MigNBFingerprintEquals(FingerprintBefore, CortexST::MakeFingerprint(Fixture.Context.StateTree)));
	TestEqual(TEXT("the binding count is untouched by the reads"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), 3);
	return true;
#endif
}
