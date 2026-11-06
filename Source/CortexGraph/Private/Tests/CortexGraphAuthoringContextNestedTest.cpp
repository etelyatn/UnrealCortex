#include "Misc/AutomationTest.h"

#include "CortexGraphTestContentRoot.h"
#include "CortexGraphTreeTestTypes.h"
#include "Operations/CortexGraphPatchState.h"

namespace CortexGraphAuthoringContextNestedTestHelpers
{
	TSharedPtr<FJsonObject> ContextRequest(UBlueprint* Blueprint, const FGuid& Guid, const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& FillRef)
	{
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("graph_guid"), Guid.ToString());
		FillRef(Ref);
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(TEXT("graph_ref"), Ref);
		TSharedPtr<FJsonObject> Params = CortexGraphTreeTest::MakeTreeParams(Blueprint->GetPathName());
		Params->SetObjectField(TEXT("target"), Target);
		return Params;
	}

	/** A patch request that adds one Self node to the graph identified by Guid. */
	TSharedPtr<FJsonObject> PatchRequest(UBlueprint* Blueprint, const FGuid& Guid, const TCHAR* PatchId)
	{
		EnsureCortexGraphTestTempContentRoot();
		TSharedPtr<FJsonObject> Request = CortexGraphTreeTest::MakeTreeParams(Blueprint->GetPathName());
		Request->SetStringField(TEXT("patch_id"), PatchId);
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("graph_guid"), Guid.ToString());
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(TEXT("graph_ref"), Ref);
		Request->SetObjectField(TEXT("target"), Target);
		Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("client_id"), TEXT("self0"));
		Node->SetStringField(TEXT("node_class"), TEXT("Self"));
		TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), 240);
		Position->SetNumberField(TEXT("y"), 120);
		Node->SetObjectField(TEXT("position"), Position);
		TArray<TSharedPtr<FJsonValue>> Nodes;
		Nodes.Add(MakeShared<FJsonValueObject>(Node));
		Request->SetArrayField(TEXT("nodes"), Nodes);
		Request->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
		Request->SetArrayField(TEXT("pin_updates"), TArray<TSharedPtr<FJsonValue>>());
		Request->SetBoolField(TEXT("dry_run"), true);
		Request->SetBoolField(TEXT("compile"), false);
		Request->SetBoolField(TEXT("save"), false);
		Request->SetBoolField(TEXT("allow_noop"), false);
		return Request;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAuthoringContextNestedReadOnlyTest,
	"Cortex.Graph.AuthoringContext.NestedReadOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphAuthoringContextNestedReadOnlyTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphTreeTest;
	using namespace CortexGraphAuthoringContextNestedTestHelpers;

	FNestedFixture Fixture;
	if (!TestTrue(TEXT("nested fixture builds"), BuildTreeNestedFixture(TEXT("BP_AuthoringContextNested"), Fixture)))
	{
		return false;
	}

	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);
	auto Context = [&](const FGuid& Guid, const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& FillRef)
	{
		return Router.Execute(TEXT("graph.get_authoring_context"), ContextRequest(Fixture.Blueprint, Guid, FillRef));
	};
	auto NoExtras = [](const TSharedPtr<FJsonObject>&) {};

	// ── A nested (non-composite) graph resolves read-only ──
	for (UEdGraph* Nested : { (UEdGraph*)Fixture.T1, (UEdGraph*)Fixture.T2, (UEdGraph*)Fixture.Grandchild, (UEdGraph*)Fixture.SubOnly })
	{
		const FCortexCommandResult Result = Context(Nested->GraphGuid, NoExtras);
		if (!TestTrue(FString::Printf(TEXT("%s resolves instead of GRAPH_NOT_FOUND: %s"), *Nested->GetName(), *Result.ErrorMessage), Result.bSuccess))
		{
			continue;
		}
		const TSharedPtr<FJsonObject>* Target = nullptr;
		if (TestTrue(TEXT("the resolved target is published"), Result.Data->TryGetObjectField(TEXT("target"), Target) && Target && Target->IsValid()))
		{
			TestEqual(TEXT("graph_guid"), (*Target)->GetStringField(TEXT("graph_guid")), TreeGuidString(Nested));
			TestEqual(TEXT("graph_name"), (*Target)->GetStringField(TEXT("graph_name")), Nested->GetName());
			TestEqual(TEXT("graph_kind is nested"), (*Target)->GetStringField(TEXT("graph_kind")), FString(TEXT("nested")));
			TestFalse(TEXT("is_mutable is false"), (*Target)->GetBoolField(TEXT("is_mutable")));
		}
		TestFalse(TEXT("a nested graph is not an authoring choice"),
			FindTreeJsonObject(Result.Data->GetArrayField(TEXT("graph_choices")), TEXT("graph_guid"), TreeGuidString(Nested)).IsValid());
	}

	// ── Composite and root targets are unchanged ──
	{
		const FCortexCommandResult Root = Context(Fixture.HostFn->GraphGuid, NoExtras);
		if (TestTrue(TEXT("root target resolves"), Root.bSuccess))
		{
			const TSharedPtr<FJsonObject> Target = Root.Data->GetObjectField(TEXT("target"));
			TestEqual(TEXT("root graph_kind unchanged"), Target->GetStringField(TEXT("graph_kind")), FString(TEXT("function")));
			TestTrue(TEXT("root stays mutable"), Target->GetBoolField(TEXT("is_mutable")));
			TestFalse(TEXT("root target gains no graph_path"), Target->HasField(TEXT("graph_path")));
		}

		const FCortexCommandResult Composite = Context(Fixture.Composite->BoundGraph->GraphGuid, NoExtras);
		if (TestTrue(TEXT("composite target resolves"), Composite.bSuccess))
		{
			const TSharedPtr<FJsonObject> Target = Composite.Data->GetObjectField(TEXT("target"));
			TestEqual(TEXT("composite keeps the root graph_kind"), Target->GetStringField(TEXT("graph_kind")), FString(TEXT("function")));
			TestTrue(TEXT("composite stays mutable"), Target->GetBoolField(TEXT("is_mutable")));
			TestEqual(TEXT("composite subgraph_path"), Target->GetStringField(TEXT("subgraph_path")), FString(TEXT("Inner")));
			TestTrue(TEXT("composite is still an authoring choice"),
				FindTreeJsonObject(Composite.Data->GetArrayField(TEXT("graph_choices")), TEXT("graph_guid"), TreeGuidString(Fixture.Composite->BoundGraph)).IsValid());
		}
	}

	// ── Identity checks still apply to nested targets ──
	{
		const FCortexCommandResult Unknown = Context(FGuid::NewGuid(), NoExtras);
		TestFalse(TEXT("an unknown GUID is refused"), Unknown.bSuccess);
		TestEqual(TEXT("unknown GUID code"), Unknown.ErrorCode, CortexErrorCodes::GraphNotFound);

		const FCortexCommandResult Kind = Context(Fixture.T1->GraphGuid, [](const TSharedPtr<FJsonObject>& Ref) { Ref->SetStringField(TEXT("graph_kind"), TEXT("function")); });
		TestEqual(TEXT("a graph_kind that disagrees with the nested graph is refused"), Kind.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult KindOk = Context(Fixture.T1->GraphGuid, [](const TSharedPtr<FJsonObject>& Ref) { Ref->SetStringField(TEXT("graph_kind"), TEXT("nested")); });
		TestTrue(TEXT("the nested kind is accepted"), KindOk.bSuccess);

		const FCortexCommandResult Path = Context(Fixture.T1->GraphGuid, [](const TSharedPtr<FJsonObject>& Ref) { Ref->SetStringField(TEXT("subgraph_path"), TEXT("Inner")); });
		TestEqual(TEXT("a subgraph_path that disagrees with the nested graph is refused"), Path.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// ── The patch route and add_node still refuse nested graphs ──
	{
		const int32 NodesBefore = Fixture.T1->Nodes.Num();
		const FCortexCommandResult Patch = Router.Execute(TEXT("graph.apply_patch"),
			PatchRequest(Fixture.Blueprint, Fixture.T1->GraphGuid, TEXT("00000000-0000-0000-0000-000000125001")));
		TestFalse(TEXT("apply_patch targeting a nested graph is refused"), Patch.bSuccess);
		TestTrue(FString::Printf(TEXT("apply_patch keeps its existing refusal code (%s)"), *Patch.ErrorCode),
			Patch.ErrorCode == CortexErrorCodes::GraphNotFound || Patch.ErrorCode == CortexErrorCodes::InvalidOperation);
		TestEqual(TEXT("the refused patch mutates nothing"), Fixture.T1->Nodes.Num(), NodesBefore);

		TSharedPtr<FJsonObject> AddParams = MakeTreeParams(Fixture.Blueprint->GetPathName());
		AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));
		AddParams->SetStringField(TEXT("graph_name"), TEXT("Transition"));
		const FCortexCommandResult Add = Router.Execute(TEXT("graph.add_node"), AddParams);
		TestFalse(TEXT("add_node addressing the nested graph by name is refused"), Add.bSuccess);
		TestEqual(TEXT("add_node code is GRAPH_NOT_FOUND"), Add.ErrorCode, CortexErrorCodes::GraphNotFound);
		TestEqual(TEXT("the refused add_node mutates nothing"), Fixture.T1->Nodes.Num(), NodesBefore);
	}

	// ── A GUID shared by two nested graphs is never guessed ──
	{
		Fixture.T2->GraphGuid = Fixture.T1->GraphGuid;
		const FCortexCommandResult Shared = Context(Fixture.T1->GraphGuid, NoExtras);
		TestFalse(TEXT("a shared nested GUID is refused"), Shared.bSuccess);
		TestEqual(TEXT("shared GUID code"), Shared.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("the refusal names both paths"),
			Shared.ErrorMessage.Contains(TEXT("HostFn/Transition")) && Shared.ErrorMessage.Contains(TEXT("HostFn/Transition~2")));
	}

	Fixture.Blueprint->MarkAsGarbage();
	return true;
}
