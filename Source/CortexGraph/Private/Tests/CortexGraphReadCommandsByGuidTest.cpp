#include "Misc/AutomationTest.h"

#include "CortexGraphTreeTestTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphReadCommandsByGuidTest,
	"Cortex.Graph.ReadCommands.ByGuid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphReadCommandsByGuidTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphTreeTest;

	FNestedFixture Fixture;
	if (!TestTrue(TEXT("nested fixture builds"), BuildTreeNestedFixture(TEXT("BP_ReadCommandsByGuid"), Fixture)))
	{
		return false;
	}

	const FString AssetPath = Fixture.Blueprint->GetPathName();
	const FString T1Guid = TreeGuidString(Fixture.T1);
	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);

	auto Run = [&](const TCHAR* Command, const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& Fill)
	{
		TSharedPtr<FJsonObject> Params = MakeTreeParams(AssetPath);
		Fill(Params);
		return Router.Execute(Command, Params);
	};
	auto Items = [](const FCortexCommandResult& Result, const TCHAR* Field) -> TArray<TSharedPtr<FJsonValue>>
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Result.Data.IsValid() && Result.Data->TryGetArrayField(Field, Array) && Array)
		{
			return *Array;
		}
		return {};
	};
	// The fresh Actor Blueprint may already carry default event nodes; the handler counts below are relative to that.
	const int32 BaselineHandlers = Items(Run(TEXT("graph.list_event_handlers"), [](const TSharedPtr<FJsonObject>&) {}), TEXT("nodes")).Num();

	// HostFn (root) and T1 both hold a PrintString call and a custom event, so the default node names collide across graphs.
	UK2Node_CallFunction* RootCall = AddTreeCallNode(Fixture.HostFn);
	UK2Node_CustomEvent* RootEvent = AddTreeEventNode(Fixture.HostFn, TEXT("RootEvent"));
	UK2Node_CallFunction* NestedCall = AddTreeCallNode(Fixture.T1);
	UK2Node_CustomEvent* NestedEvent = AddTreeEventNode(Fixture.T1, TEXT("NestedEvent"));
	NestedEvent->Rename(TEXT("NestedOnlyEvent"));
	UK2Node_CallFunction* GrandCall = AddTreeCallNode(Fixture.Grandchild);
	NestedEvent->FindPinChecked(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(NestedCall->GetExecPin());
	TestEqual(TEXT("the root and nested call nodes share a default node name"), RootCall->GetName(), NestedCall->GetName());
	TestNotNull(TEXT("root event"), RootEvent);
	TestNotNull(TEXT("grandchild call"), GrandCall);

	// Every item of Array carries graph_guid; returns how many of them sit in the graph with GraphGuid.
	auto CountInGraph = [&](const TArray<TSharedPtr<FJsonValue>>& Array, const UEdGraph* Graph)
	{
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& Item : Array)
		{
			FString Guid;
			if (TestTrue(TEXT("every result carries graph_guid"), Item->AsObject()->TryGetStringField(TEXT("graph_guid"), Guid)) && Guid == TreeGuidString(Graph))
			{
				++Count;
			}
		}
		return Count;
	};

	// ── search_nodes ──
	{
		const FCortexCommandResult All = Run(TEXT("graph.search_nodes"), [](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("function_name"), TEXT("PrintString")); });
		const TArray<TSharedPtr<FJsonValue>> AllItems = Items(All, TEXT("results"));
		TestEqual(TEXT("an unscoped search also finds the nested call nodes"), AllItems.Num(), 3);
		TestEqual(TEXT("one root hit"), CountInGraph(AllItems, Fixture.HostFn), 1);
		TestEqual(TEXT("one T1 hit"), CountInGraph(AllItems, Fixture.T1), 1);
		TestEqual(TEXT("one Grandchild hit"), CountInGraph(AllItems, Fixture.Grandchild), 1);

		const FCortexCommandResult Scoped = Run(TEXT("graph.search_nodes"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("function_name"), TEXT("PrintString"));
			P->SetStringField(TEXT("graph_guid"), T1Guid);
		});
		const TArray<TSharedPtr<FJsonValue>> ScopedItems = Items(Scoped, TEXT("results"));
		TestEqual(TEXT("graph_guid scopes the search to that graph's subtree"), ScopedItems.Num(), 2);
		TestEqual(TEXT("the T1 call is in the scope"), CountInGraph(ScopedItems, Fixture.T1), 1);
		TestEqual(TEXT("the grandchild call is in the scope"), CountInGraph(ScopedItems, Fixture.Grandchild), 1);

		const FCortexCommandResult Leaf = Run(TEXT("graph.search_nodes"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("function_name"), TEXT("PrintString"));
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.Grandchild));
		});
		TestEqual(TEXT("a leaf graph scopes to its own nodes"), CountInGraph(Items(Leaf, TEXT("results")), Fixture.Grandchild), 1);
		TestEqual(TEXT("and nothing else"), Items(Leaf, TEXT("results")).Num(), 1);

		const FCortexCommandResult ByPath = Run(TEXT("graph.search_nodes"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("function_name"), TEXT("PrintString"));
			P->SetStringField(TEXT("graph_path"), TEXT("HostFn/Transition"));
		});
		TestEqual(TEXT("graph_path scopes like graph_guid"), Items(ByPath, TEXT("results")).Num(), 2);

		const FCortexCommandResult Empty = Run(TEXT("graph.search_nodes"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("function_name"), TEXT("PrintString"));
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.T2));
		});
		TestTrue(TEXT("a scope without matches succeeds"), Empty.bSuccess);
		TestEqual(TEXT("and is empty"), Items(Empty, TEXT("results")).Num(), 0);

		const FCortexCommandResult Unknown = Run(TEXT("graph.search_nodes"), [](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString()); });
		TestFalse(TEXT("search_nodes with an unknown graph_guid refuses"), Unknown.bSuccess);
		TestEqual(TEXT("search_nodes unknown guid code"), Unknown.ErrorCode, CortexErrorCodes::GraphNotFound);
	}

	// ── list_event_handlers ──
	{
		const FCortexCommandResult All = Run(TEXT("graph.list_event_handlers"), [](const TSharedPtr<FJsonObject>&) {});
		const TArray<TSharedPtr<FJsonValue>> AllItems = Items(All, TEXT("nodes"));
		TestEqual(TEXT("an unscoped listing finds the root and the nested event"), AllItems.Num(), BaselineHandlers + 2);
		TestEqual(TEXT("the nested event reports its graph"), CountInGraph(AllItems, Fixture.T1), 1);

		const FCortexCommandResult Scoped = Run(TEXT("graph.list_event_handlers"), [&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_guid"), T1Guid); });
		const TArray<TSharedPtr<FJsonValue>> ScopedItems = Items(Scoped, TEXT("nodes"));
		TestEqual(TEXT("graph_guid scopes list_event_handlers"), ScopedItems.Num(), 1);
		TestEqual(TEXT("the one handler is T1's"), CountInGraph(ScopedItems, Fixture.T1), 1);

		const FCortexCommandResult Rootless = Run(TEXT("graph.list_event_handlers"), [&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.T2)); });
		TestEqual(TEXT("a graph without handlers lists none"), Items(Rootless, TEXT("nodes")).Num(), 0);

		const FCortexCommandResult Unknown = Run(TEXT("graph.list_event_handlers"), [](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString()); });
		TestEqual(TEXT("list_event_handlers unknown guid code"), Unknown.ErrorCode, CortexErrorCodes::GraphNotFound);
	}

	// ── find_event_handler ──
	{
		const FCortexCommandResult All = Run(TEXT("graph.find_event_handler"), [](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("event_name"), TEXT("NestedEvent")); });
		TestEqual(TEXT("an unscoped lookup finds the nested event by name"), Items(All, TEXT("nodes")).Num(), 1);

		const FCortexCommandResult Scoped = Run(TEXT("graph.find_event_handler"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("event_name"), TEXT("Event"));
			P->SetStringField(TEXT("graph_guid"), T1Guid);
		});
		const TArray<TSharedPtr<FJsonValue>> ScopedItems = Items(Scoped, TEXT("nodes"));
		TestEqual(TEXT("graph_guid scopes find_event_handler (the root event also matches 'Event')"), ScopedItems.Num(), 1);
		TestEqual(TEXT("the match is T1's"), CountInGraph(ScopedItems, Fixture.T1), 1);

		const FCortexCommandResult Unknown = Run(TEXT("graph.find_event_handler"), [](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("event_name"), TEXT("Event"));
			P->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString());
		});
		TestEqual(TEXT("find_event_handler unknown guid code"), Unknown.ErrorCode, CortexErrorCodes::GraphNotFound);
	}

	// ── find_function_calls ──
	{
		const FCortexCommandResult All = Run(TEXT("graph.find_function_calls"), [](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("function_name"), TEXT("PrintString")); });
		TestEqual(TEXT("an unscoped lookup finds the nested calls"), Items(All, TEXT("nodes")).Num(), 3);

		const FCortexCommandResult Scoped = Run(TEXT("graph.find_function_calls"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("function_name"), TEXT("PrintString"));
			P->SetStringField(TEXT("graph_guid"), T1Guid);
		});
		const TArray<TSharedPtr<FJsonValue>> ScopedItems = Items(Scoped, TEXT("nodes"));
		TestEqual(TEXT("graph_guid scopes find_function_calls to the subtree"), ScopedItems.Num(), 2);
		TestEqual(TEXT("T1's call"), CountInGraph(ScopedItems, Fixture.T1), 1);
		TestEqual(TEXT("the grandchild's call"), CountInGraph(ScopedItems, Fixture.Grandchild), 1);

		const FCortexCommandResult Unknown = Run(TEXT("graph.find_function_calls"), [](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("function_name"), TEXT("PrintString"));
			P->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString());
		});
		TestEqual(TEXT("find_function_calls unknown guid code"), Unknown.ErrorCode, CortexErrorCodes::GraphNotFound);
	}

	// ── trace_exec / trace_dataflow ──
	{
		const FCortexCommandResult Exec = Run(TEXT("graph.trace_exec"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), T1Guid);
			P->SetStringField(TEXT("start_node_id"), NestedEvent->GetName());
		});
		if (TestTrue(TEXT("trace_exec by graph_guid succeeds"), Exec.bSuccess))
		{
			const TArray<TSharedPtr<FJsonValue>> Nodes = Items(Exec, TEXT("nodes"));
			TestEqual(TEXT("trace_exec follows the edge inside the nested graph"), Nodes.Num(), 2);
			TestEqual(TEXT("both traced nodes report the nested graph"), CountInGraph(Nodes, Fixture.T1), 2);
			TestEqual(TEXT("one edge is reported"), Items(Exec, TEXT("edges")).Num(), 1);
		}

		// The default node name exists in the root graph and in T1: the graph scope decides.
		const FCortexCommandResult Scoped = Run(TEXT("graph.trace_exec"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), T1Guid);
			P->SetStringField(TEXT("start_node_id"), NestedCall->GetName());
		});
		TestEqual(TEXT("a shared node name resolves inside the addressed graph"), CountInGraph(Items(Scoped, TEXT("nodes")), Fixture.T1), 1);

		const FCortexCommandResult Unscoped = Run(TEXT("graph.trace_exec"), [&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("start_node_id"), NestedEvent->GetName()); });
		TestTrue(TEXT("an unscoped trace finds a node that only exists in a nested graph"), Unscoped.bSuccess);

		const FCortexCommandResult Dataflow = Run(TEXT("graph.trace_dataflow"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), T1Guid);
			P->SetStringField(TEXT("start_node_id"), NestedCall->GetName());
		});
		if (TestTrue(TEXT("trace_dataflow by graph_guid succeeds"), Dataflow.bSuccess))
		{
			TestEqual(TEXT("trace_dataflow starts in the addressed graph"), CountInGraph(Items(Dataflow, TEXT("nodes")), Fixture.T1), 1);
		}

		const FCortexCommandResult WrongGraph = Run(TEXT("graph.trace_exec"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.T2));
			P->SetStringField(TEXT("start_node_id"), NestedEvent->GetName());
		});
		TestEqual(TEXT("a node that is not in the addressed graph is NODE_NOT_FOUND"), WrongGraph.ErrorCode, CortexErrorCodes::NodeNotFound);

		const FCortexCommandResult Unknown = Run(TEXT("graph.trace_exec"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString());
			P->SetStringField(TEXT("start_node_id"), NestedEvent->GetName());
		});
		TestEqual(TEXT("trace_exec unknown guid code"), Unknown.ErrorCode, CortexErrorCodes::GraphNotFound);
		const FCortexCommandResult UnknownFlow = Run(TEXT("graph.trace_dataflow"), [&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString());
			P->SetStringField(TEXT("start_node_id"), NestedEvent->GetName());
		});
		TestEqual(TEXT("trace_dataflow unknown guid code"), UnknownFlow.ErrorCode, CortexErrorCodes::GraphNotFound);
	}

	Fixture.Blueprint->MarkAsGarbage();
	return true;
}
