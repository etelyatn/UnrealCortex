#include "Misc/AutomationTest.h"

#include "CortexGraphTestContentRoot.h"
#include "CortexGraphTreeTestTypes.h"

namespace CortexGraphNestedFixSupport
{
	using namespace CortexGraphTreeTest;

	TSharedPtr<FJsonObject> GraphRef(const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& Fill)
	{
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Fill(Ref);
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(TEXT("graph_ref"), Ref);
		return Target;
	}

	FCortexCommandResult Context(FCortexCommandRouter& Router, UBlueprint* Blueprint, const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& FillRef)
	{
		TSharedPtr<FJsonObject> Params = MakeTreeParams(Blueprint->GetPathName());
		Params->SetObjectField(TEXT("target"), GraphRef(FillRef));
		return Router.Execute(TEXT("graph.get_authoring_context"), Params);
	}

	FCortexCommandResult Describe(FCortexCommandRouter& Router, UBlueprint* Blueprint, const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& FillRef)
	{
		TSharedPtr<FJsonObject> Params = MakeTreeParams(Blueprint->GetPathName());
		Params->SetStringField(TEXT("node_class"), TEXT("Self"));
		Params->SetObjectField(TEXT("target"), GraphRef(FillRef));
		return Router.Execute(TEXT("graph.describe_node"), Params);
	}

	TSharedPtr<FJsonObject> ContextTarget(const FCortexCommandResult& Result)
	{
		return Result.Data.IsValid() && Result.Data->HasTypedField<EJson::Object>(TEXT("target")) ? Result.Data->GetObjectField(TEXT("target")) : nullptr;
	}

	TSharedPtr<FJsonObject> DescribeTarget(const FCortexCommandResult& Result)
	{
		if (!Result.Data.IsValid() || !Result.Data->HasTypedField<EJson::Object>(TEXT("context")))
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject> ContextObject = Result.Data->GetObjectField(TEXT("context"));
		return ContextObject->HasTypedField<EJson::Object>(TEXT("target")) ? ContextObject->GetObjectField(TEXT("target")) : nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> Array(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values)
		{
			return *Values;
		}
		return {};
	}
}

// ── F1: graph_path in contextual reads ──────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAuthoringContextNestedGraphPathTest,
	"Cortex.Graph.AuthoringContext.NestedGraphPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphAuthoringContextNestedGraphPathTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNestedFixSupport;

	FNestedFixture Fixture;
	if (!TestTrue(TEXT("nested fixture builds"), BuildTreeNestedFixture(TEXT("BP_NestedGraphPathContext"), Fixture)))
	{
		return false;
	}
	// A pasted state graph keeps its GUID: both Transition graphs now share one GraphGuid.
	Fixture.T2->GraphGuid = Fixture.T1->GraphGuid;
	const FString SharedGuid = Fixture.T1->GraphGuid.ToString();

	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);

	// ── get_authoring_context ──
	{
		const FCortexCommandResult Shared = Context(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R) { R->SetStringField(TEXT("graph_guid"), SharedGuid); });
		TestFalse(TEXT("a shared GUID alone is still refused"), Shared.bSuccess);
		TestEqual(TEXT("shared GUID code"), Shared.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult Second = Context(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R)
		{
			R->SetStringField(TEXT("graph_guid"), SharedGuid);
			R->SetStringField(TEXT("graph_path"), TEXT("HostFn/Transition~2"));
		});
		if (TestTrue(FString::Printf(TEXT("GUID plus graph_path selects the second colliding graph: %s"), *Second.ErrorMessage), Second.bSuccess))
		{
			const TSharedPtr<FJsonObject> Target = ContextTarget(Second);
			if (TestTrue(TEXT("target published"), Target.IsValid()))
			{
				TestEqual(TEXT("selected graph_path"), Target->GetStringField(TEXT("graph_path")), FString(TEXT("HostFn/Transition~2")));
				TestFalse(TEXT("nested graph is read-only"), Target->GetBoolField(TEXT("is_mutable")));
			}
		}

		const FCortexCommandResult PathOnly = Context(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R) { R->SetStringField(TEXT("graph_path"), TEXT("HostFn/Transition")); });
		if (TestTrue(FString::Printf(TEXT("a path-only graph_ref resolves: %s"), *PathOnly.ErrorMessage), PathOnly.bSuccess))
		{
			const TSharedPtr<FJsonObject> Target = ContextTarget(PathOnly);
			if (TestTrue(TEXT("target published"), Target.IsValid()))
			{
				TestEqual(TEXT("path-only graph_path"), Target->GetStringField(TEXT("graph_path")), FString(TEXT("HostFn/Transition")));
				TestEqual(TEXT("path-only parent"), Target->GetStringField(TEXT("parent_graph_guid")), TreeGuidString(Fixture.HostFn));
			}
		}

		const FCortexCommandResult Disagree = Context(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R)
		{
			R->SetStringField(TEXT("graph_guid"), SharedGuid);
			R->SetStringField(TEXT("graph_path"), TEXT("HostFn/SubOnly"));
		});
		TestEqual(TEXT("a GUID that does not belong to the graph_path is refused"), Disagree.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult Unknown = Context(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R) { R->SetStringField(TEXT("graph_path"), TEXT("HostFn/Nope")); });
		TestEqual(TEXT("an unknown graph_path is GRAPH_NOT_FOUND"), Unknown.ErrorCode, CortexErrorCodes::GraphNotFound);

		const FCortexCommandResult Empty = Context(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>&) {});
		TestEqual(TEXT("a graph_ref with neither graph_guid nor graph_path is INVALID_FIELD"), Empty.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult Root = Context(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R) { R->SetStringField(TEXT("graph_path"), TEXT("HostFn")); });
		if (TestTrue(TEXT("a root graph_path resolves"), Root.bSuccess))
		{
			const TSharedPtr<FJsonObject> Target = ContextTarget(Root);
			if (TestTrue(TEXT("target published"), Target.IsValid()))
			{
				TestTrue(TEXT("the root graph stays mutable"), Target->GetBoolField(TEXT("is_mutable")));
				TestEqual(TEXT("root graph_kind"), Target->GetStringField(TEXT("graph_kind")), FString(TEXT("function")));
				TestEqual(TEXT("root graph_guid"), Target->GetStringField(TEXT("graph_guid")), TreeGuidString(Fixture.HostFn));
			}
		}
	}

	// ── describe_node ──
	{
		const FCortexCommandResult Shared = Describe(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R) { R->SetStringField(TEXT("graph_guid"), SharedGuid); });
		TestEqual(TEXT("describe_node: a shared GUID alone is refused"), Shared.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult Second = Describe(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R)
		{
			R->SetStringField(TEXT("graph_guid"), SharedGuid);
			R->SetStringField(TEXT("graph_path"), TEXT("HostFn/Transition~2"));
		});
		if (TestTrue(FString::Printf(TEXT("describe_node: GUID plus graph_path resolves: %s"), *Second.ErrorMessage), Second.bSuccess))
		{
			const TSharedPtr<FJsonObject> Target = DescribeTarget(Second);
			if (TestTrue(TEXT("describe target published"), Target.IsValid()))
			{
				TestEqual(TEXT("describe selected graph_path"), Target->GetStringField(TEXT("graph_path")), FString(TEXT("HostFn/Transition~2")));
				TestFalse(TEXT("describe nested target is read-only"), Target->GetBoolField(TEXT("is_mutable")));
			}
		}

		const FCortexCommandResult PathOnly = Describe(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R) { R->SetStringField(TEXT("graph_path"), TEXT("HostFn/Transition")); });
		if (TestTrue(FString::Printf(TEXT("describe_node: a path-only graph_ref resolves: %s"), *PathOnly.ErrorMessage), PathOnly.bSuccess))
		{
			const TSharedPtr<FJsonObject> Target = DescribeTarget(PathOnly);
			TestTrue(TEXT("describe path-only target"), Target.IsValid() && Target->GetStringField(TEXT("graph_path")) == TEXT("HostFn/Transition"));
		}

		const FCortexCommandResult Disagree = Describe(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R)
		{
			R->SetStringField(TEXT("graph_guid"), SharedGuid);
			R->SetStringField(TEXT("graph_path"), TEXT("HostFn/SubOnly"));
		});
		TestEqual(TEXT("describe_node: a GUID that does not belong to the graph_path is refused"), Disagree.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult Unknown = Describe(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R) { R->SetStringField(TEXT("graph_path"), TEXT("HostFn/Nope")); });
		TestEqual(TEXT("describe_node: an unknown graph_path is GRAPH_NOT_FOUND"), Unknown.ErrorCode, CortexErrorCodes::GraphNotFound);
	}

	Fixture.Blueprint->MarkAsGarbage();
	return true;
}

// ── F3: contextual describe_node validates nested selectors ─────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeNodeNestedSelectorsTest,
	"Cortex.Graph.DescribeNode.NestedSelectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphDescribeNodeNestedSelectorsTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNestedFixSupport;

	FNestedFixture Fixture;
	if (!TestTrue(TEXT("nested fixture builds"), BuildTreeNestedFixture(TEXT("BP_DescribeNestedSelectors"), Fixture)))
	{
		return false;
	}
	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);
	const FString T1Guid = Fixture.T1->GraphGuid.ToString();

	const FCortexCommandResult Plain = Describe(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R) { R->SetStringField(TEXT("graph_guid"), T1Guid); });
	if (TestTrue(FString::Printf(TEXT("a nested GUID alone describes: %s"), *Plain.ErrorMessage), Plain.bSuccess))
	{
		TestEqual(TEXT("validated"), Plain.Data->GetStringField(TEXT("validation_status")), FString(TEXT("validated")));
		const TSharedPtr<FJsonObject> Target = DescribeTarget(Plain);
		TestTrue(TEXT("nested kind reported"), Target.IsValid() && Target->GetStringField(TEXT("graph_kind")) == TEXT("nested"));
	}

	const FCortexCommandResult Kind = Describe(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R)
	{
		R->SetStringField(TEXT("graph_guid"), T1Guid);
		R->SetStringField(TEXT("graph_kind"), TEXT("function"));
	});
	TestFalse(TEXT("a graph_kind that disagrees with the nested graph is refused"), Kind.bSuccess);
	TestEqual(TEXT("graph_kind mismatch code"), Kind.ErrorCode, CortexErrorCodes::InvalidField);

	const FCortexCommandResult KindOk = Describe(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R)
	{
		R->SetStringField(TEXT("graph_guid"), T1Guid);
		R->SetStringField(TEXT("graph_kind"), TEXT("nested"));
	});
	TestTrue(TEXT("the nested kind is accepted"), KindOk.bSuccess);

	const FCortexCommandResult Path = Describe(Router, Fixture.Blueprint, [&](const TSharedPtr<FJsonObject>& R)
	{
		R->SetStringField(TEXT("graph_guid"), T1Guid);
		R->SetStringField(TEXT("subgraph_path"), TEXT("Inner"));
	});
	TestFalse(TEXT("a subgraph_path that names a different graph is refused"), Path.bSuccess);
	TestEqual(TEXT("subgraph_path mismatch code"), Path.ErrorCode, CortexErrorCodes::InvalidField);

	// A flat target (no graph_ref) is validated the same way.
	{
		TSharedPtr<FJsonObject> Params = MakeTreeParams(Fixture.Blueprint->GetPathName());
		Params->SetStringField(TEXT("node_class"), TEXT("Self"));
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("graph_guid"), T1Guid);
		Target->SetStringField(TEXT("graph_kind"), TEXT("function"));
		Params->SetObjectField(TEXT("target"), Target);
		TestEqual(TEXT("a flat target is validated the same way"), Router.Execute(TEXT("graph.describe_node"), Params).ErrorCode, CortexErrorCodes::InvalidField);
	}

	Fixture.Blueprint->MarkAsGarbage();
	return true;
}

// ── F2: selector agreement in the handler and call reads ────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphReadCommandsSelectorAgreementTest,
	"Cortex.Graph.ReadCommands.SelectorAgreement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphReadCommandsSelectorAgreementTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNestedFixSupport;

	FNestedFixture Fixture;
	if (!TestTrue(TEXT("nested fixture builds"), BuildTreeNestedFixture(TEXT("BP_SelectorAgreement"), Fixture)))
	{
		return false;
	}
	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);
	const FString T1Guid = Fixture.T1->GraphGuid.ToString();

	struct FCommandCase
	{
		const TCHAR* Command;
		const TCHAR* RequiredField;
		const TCHAR* RequiredValue;
	};
	const FCommandCase Cases[] = {
		{ TEXT("graph.list_event_handlers"), nullptr, nullptr },
		{ TEXT("graph.find_event_handler"), TEXT("event_name"), TEXT("Event") },
		{ TEXT("graph.find_function_calls"), TEXT("function_name"), TEXT("PrintString") },
	};

	for (const FCommandCase& Case : Cases)
	{
		auto Run = [&](const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& Fill)
		{
			TSharedPtr<FJsonObject> Params = MakeTreeParams(Fixture.Blueprint->GetPathName());
			if (Case.RequiredField != nullptr)
			{
				Params->SetStringField(Case.RequiredField, Case.RequiredValue);
			}
			Fill(Params);
			return Router.Execute(Case.Command, Params);
		};
		const FString Label = Case.Command;

		const FCortexCommandResult Name = Run([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), T1Guid);
			P->SetStringField(TEXT("graph_name"), TEXT("Grandchild"));
		});
		TestFalse(FString::Printf(TEXT("%s: graph_guid with a disagreeing graph_name is refused"), *Label), Name.bSuccess);
		TestEqual(FString::Printf(TEXT("%s: graph_name disagreement code"), *Label), Name.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult Sub = Run([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), T1Guid);
			P->SetStringField(TEXT("subgraph_path"), TEXT("Inner"));
		});
		TestEqual(FString::Printf(TEXT("%s: graph_guid with a disagreeing subgraph_path is refused"), *Label), Sub.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult PathName = Run([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_path"), TEXT("HostFn/Transition"));
			P->SetStringField(TEXT("graph_name"), TEXT("Grandchild"));
		});
		TestEqual(FString::Printf(TEXT("%s: graph_path with a disagreeing graph_name is refused"), *Label), PathName.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult Agreeing = Run([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), T1Guid);
			P->SetStringField(TEXT("graph_name"), TEXT("Transition"));
		});
		TestTrue(FString::Printf(TEXT("%s: an agreeing graph_name is accepted"), *Label), Agreeing.bSuccess);

		// Without an identity the old meaning stands: graph_name was never a scope for these commands.
		const FCortexCommandResult Unscoped = Run([&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_name"), TEXT("NoSuchGraphAnywhere")); });
		TestTrue(FString::Printf(TEXT("%s: graph_name without an identity stays ignored"), *Label), Unscoped.bSuccess);
	}

	Fixture.Blueprint->MarkAsGarbage();
	return true;
}

// ── F4: a cut catalogue is reported by every scanning read ──────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphReadCommandsTruncatedTreeTest,
	"Cortex.Graph.ReadCommands.TruncatedTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphReadCommandsTruncatedTreeTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNestedFixSupport;

	UBlueprint* Blueprint = CreateTreeBlueprint(TEXT("BP_TruncatedReads"));
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	// DeepFn -> Level1 -> ... -> Level10; Level9 sits below the depth-8 cut and holds the only call and the only event.
	UEdGraph* Root = AddTreeFunctionGraph(Blueprint, TEXT("DeepFn"));
	UEdGraph* Parent = Root;
	TArray<UCortexGraphTestNestedGraph*> Chain;
	for (int32 Level = 1; Level <= 10; ++Level)
	{
		UCortexGraphTestNestedGraph* Child = NewTreeNestedGraph(Parent, *FString::Printf(TEXT("Level%d"), Level));
		AddTreeHostNode(Parent, { Child });
		Chain.Add(Child);
		Parent = Child;
	}
	AddTreeCallNode(Chain[8]);
	AddTreeEventNode(Chain[8], TEXT("DeepEvent"));
	const FString CutGuid = Chain[7]->GraphGuid.ToString();

	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);

	auto ExpectCut = [&](const TCHAR* Command, const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& Fill, const TCHAR* ResultField, int32 ExpectedCount)
	{
		TSharedPtr<FJsonObject> Params = MakeTreeParams(Blueprint->GetPathName());
		Fill(Params);
		const FCortexCommandResult Result = Router.Execute(Command, Params);
		const FString Label = Command;
		if (!TestTrue(FString::Printf(TEXT("%s succeeds"), *Label), Result.bSuccess) || !Result.Data.IsValid())
		{
			return;
		}
		TestEqual(FString::Printf(TEXT("%s finds nothing below the cut"), *Label), Array(Result.Data, ResultField).Num(), ExpectedCount);
		bool bTruncated = false;
		TestTrue(FString::Printf(TEXT("%s reports truncated"), *Label), Result.Data->TryGetBoolField(TEXT("truncated"), bTruncated) && bTruncated);
		const TArray<TSharedPtr<FJsonValue>> Cut = Array(Result.Data, TEXT("truncated_parent_graph_guids"));
		if (TestEqual(FString::Printf(TEXT("%s names exactly one cut parent"), *Label), Cut.Num(), 1))
		{
			TestEqual(FString::Printf(TEXT("%s names Level8"), *Label), Cut[0]->AsString(), CutGuid);
		}
	};

	ExpectCut(TEXT("graph.search_nodes"), [](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("function_name"), TEXT("PrintString")); }, TEXT("results"), 0);
	ExpectCut(TEXT("graph.search_nodes"), [&](const TSharedPtr<FJsonObject>& P)
	{
		P->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		P->SetStringField(TEXT("graph_guid"), Chain[0]->GraphGuid.ToString());
	}, TEXT("results"), 0);
	ExpectCut(TEXT("graph.find_function_calls"), [](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("function_name"), TEXT("PrintString")); }, TEXT("nodes"), 0);
	ExpectCut(TEXT("graph.find_event_handler"), [](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("event_name"), TEXT("DeepEvent")); }, TEXT("nodes"), 0);

	{
		// list_event_handlers: the Actor Blueprint may carry default events; the one below the cut must be the only thing missing.
		TSharedPtr<FJsonObject> Params = MakeTreeParams(Blueprint->GetPathName());
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.list_event_handlers"), Params);
		if (TestTrue(TEXT("list_event_handlers succeeds"), Result.bSuccess) && Result.Data.IsValid())
		{
			TestNull(TEXT("the event below the cut is not listed"),
				FindTreeJsonObject(Array(Result.Data, TEXT("nodes")), TEXT("graph_name"), TEXT("Level9")).Get());
			bool bTruncated = false;
			TestTrue(TEXT("list_event_handlers reports truncated"), Result.Data->TryGetBoolField(TEXT("truncated"), bTruncated) && bTruncated);
			const TArray<TSharedPtr<FJsonValue>> Cut = Array(Result.Data, TEXT("truncated_parent_graph_guids"));
			TestTrue(TEXT("list_event_handlers names Level8"), Cut.Num() == 1 && Cut[0]->AsString() == CutGuid);
		}
	}

	// A tree that is not cut adds no field.
	{
		FNestedFixture Shallow;
		if (TestTrue(TEXT("shallow fixture builds"), BuildTreeNestedFixture(TEXT("BP_NotTruncatedReads"), Shallow)))
		{
			TSharedPtr<FJsonObject> Params = MakeTreeParams(Shallow.Blueprint->GetPathName());
			Params->SetStringField(TEXT("function_name"), TEXT("PrintString"));
			const FCortexCommandResult Search = Router.Execute(TEXT("graph.search_nodes"), Params);
			TestTrue(TEXT("shallow search succeeds"), Search.bSuccess);
			TestFalse(TEXT("search_nodes adds no truncated field for a complete tree"), Search.Data.IsValid() && Search.Data->HasField(TEXT("truncated")));
			TestFalse(TEXT("search_nodes adds no cut-parent list for a complete tree"), Search.Data.IsValid() && Search.Data->HasField(TEXT("truncated_parent_graph_guids")));

			const FCortexCommandResult Handlers = Router.Execute(TEXT("graph.list_event_handlers"), MakeTreeParams(Shallow.Blueprint->GetPathName()));
			TestFalse(TEXT("list_event_handlers adds no truncated field for a complete tree"), Handlers.Data.IsValid() && Handlers.Data->HasField(TEXT("truncated")));
			Shallow.Blueprint->MarkAsGarbage();
		}
	}

	Blueprint->MarkAsGarbage();
	return true;
}

// ── add_node never silently ignores graph_guid ──────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAddNodeGraphGuidRefusedTest,
	"Cortex.Graph.AddNode.GraphGuidRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphAddNodeGraphGuidRefusedTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNestedFixSupport;

	FNestedFixture Fixture;
	if (!TestTrue(TEXT("nested fixture builds"), BuildTreeNestedFixture(TEXT("BP_AddNodeGraphGuid"), Fixture)))
	{
		return false;
	}
	EnsureCortexGraphTestTempContentRoot();
	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);

	UEdGraph* EventGraph = Fixture.Blueprint->UbergraphPages[0];
	auto TotalNodes = [&]() { return EventGraph->Nodes.Num() + Fixture.HostFn->Nodes.Num() + Fixture.T1->Nodes.Num() + Fixture.Composite->BoundGraph->Nodes.Num(); };
	const int32 NodesBefore = TotalNodes();

	auto AddWith = [&](const FString& Guid)
	{
		TSharedPtr<FJsonObject> Params = MakeTreeParams(Fixture.Blueprint->GetPathName());
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));
		Params->SetStringField(TEXT("graph_guid"), Guid);
		return Router.Execute(TEXT("graph.add_node"), Params);
	};

	const FCortexCommandResult Nested = AddWith(Fixture.T1->GraphGuid.ToString());
	TestFalse(TEXT("add_node with a nested graph_guid is refused"), Nested.bSuccess);
	TestEqual(TEXT("nested graph_guid is INVALID_OPERATION"), Nested.ErrorCode, CortexErrorCodes::InvalidOperation);

	const FCortexCommandResult Root = AddWith(Fixture.HostFn->GraphGuid.ToString());
	TestFalse(TEXT("add_node with a root graph_guid is refused instead of ignored"), Root.bSuccess);
	TestEqual(TEXT("root graph_guid is INVALID_FIELD (addressing is graph_name/subgraph_path)"), Root.ErrorCode, CortexErrorCodes::InvalidField);

	const FCortexCommandResult Composite = AddWith(Fixture.Composite->BoundGraph->GraphGuid.ToString());
	TestFalse(TEXT("add_node with a composite graph_guid is refused instead of ignored"), Composite.bSuccess);
	TestEqual(TEXT("composite graph_guid is INVALID_FIELD"), Composite.ErrorCode, CortexErrorCodes::InvalidField);

	TestEqual(TEXT("unknown graph_guid is GRAPH_NOT_FOUND"), AddWith(FGuid::NewGuid().ToString()).ErrorCode, CortexErrorCodes::GraphNotFound);
	TestEqual(TEXT("malformed graph_guid is INVALID_FIELD"), AddWith(TEXT("not-a-guid")).ErrorCode, CortexErrorCodes::InvalidField);

	TestEqual(TEXT("no refused call mutated any graph (nothing defaulted to EventGraph)"), TotalNodes(), NodesBefore);

	// graph_name addressing is untouched.
	TSharedPtr<FJsonObject> ByName = MakeTreeParams(Fixture.Blueprint->GetPathName());
	ByName->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));
	ByName->SetStringField(TEXT("graph_name"), TEXT("HostFn"));
	TestTrue(TEXT("add_node by graph_name still works"), Router.Execute(TEXT("graph.add_node"), ByName).bSuccess);

	Fixture.Blueprint->MarkAsGarbage();
	return true;
}
