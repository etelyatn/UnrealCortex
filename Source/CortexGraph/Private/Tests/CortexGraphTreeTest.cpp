#include "Misc/AutomationTest.h"

#include "CortexGraphTreeTestTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphTreeListNestedTest,
	"Cortex.Graph.GraphTree.ListNested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphTreeListNestedTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphTreeTest;

	FNestedFixture Fixture;
	if (!TestTrue(TEXT("nested fixture builds"), BuildTreeNestedFixture(TEXT("BP_GraphTreeListNested"), Fixture)))
	{
		return false;
	}

	const FString AssetPath = Fixture.Blueprint->GetPathName();
	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);

	auto ListGraphs = [&](const TSharedPtr<FJsonObject>& Params) { return Router.Execute(TEXT("graph.list_graphs"), Params); };
	auto GraphsOf = [](const FCortexCommandResult& Result) -> TArray<TSharedPtr<FJsonValue>>
	{
		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("graphs"), Graphs) && Graphs)
		{
			return *Graphs;
		}
		return {};
	};

	// ── Default call: unchanged entry set and kinds, plus graph_guid / path / graph_guid_unique ──
	{
		const FCortexCommandResult Result = ListGraphs(MakeTreeParams(AssetPath));
		TestTrue(TEXT("default list_graphs succeeds"), Result.bSuccess);
		const TArray<TSharedPtr<FJsonValue>> Graphs = GraphsOf(Result);
		// EventGraph, UserConstructionScript (an Actor Blueprint always has both) and HostFn.
		TestEqual(TEXT("default call lists only the root graphs"), Graphs.Num(), 3);

		const TSharedPtr<FJsonObject> Event = FindTreeJsonObject(Graphs, TEXT("name"), TEXT("EventGraph"));
		const TSharedPtr<FJsonObject> Host = FindTreeJsonObject(Graphs, TEXT("name"), TEXT("HostFn"));
		TestTrue(TEXT("EventGraph listed"), Event.IsValid());
		TestTrue(TEXT("HostFn listed"), Host.IsValid());
		for (const TSharedPtr<FJsonObject>& Root : { Event, Host })
		{
			if (!Root.IsValid())
			{
				continue;
			}
			const FString Name = Root->GetStringField(TEXT("name"));
			TestTrue(FString::Printf(TEXT("%s has graph_guid"), *Name), Root->HasField(TEXT("graph_guid")));
			TestEqual(FString::Printf(TEXT("%s path is its name"), *Name), Root->GetStringField(TEXT("path")), Name);
			TestTrue(FString::Printf(TEXT("%s graph_guid_unique"), *Name), Root->GetBoolField(TEXT("graph_guid_unique")));
			TestFalse(FString::Printf(TEXT("%s has no parent_graph_guid"), *Name), Root->HasField(TEXT("parent_graph_guid")));
			TestFalse(FString::Printf(TEXT("%s has no owner_node_guid"), *Name), Root->HasField(TEXT("owner_node_guid")));
			TestFalse(FString::Printf(TEXT("%s has no graph_class_kind (not an AnimationGraph)"), *Name), Root->HasField(TEXT("graph_class_kind")));
		}
		if (Event.IsValid() && Host.IsValid())
		{
			TestEqual(TEXT("EventGraph kind unchanged"), Event->GetStringField(TEXT("kind")), FString(TEXT("ubergraph")));
			TestEqual(TEXT("HostFn kind unchanged"), Host->GetStringField(TEXT("kind")), FString(TEXT("function")));
			TestEqual(TEXT("HostFn graph_guid"), Host->GetStringField(TEXT("graph_guid")), TreeGuidString(Fixture.HostFn));
		}
	}

	// ── include_subgraphs: nested graphs, composite children, deterministic output ──
	TSharedPtr<FJsonObject> IncludeParams = MakeTreeParams(AssetPath);
	IncludeParams->SetBoolField(TEXT("include_subgraphs"), true);
	const FCortexCommandResult First = ListGraphs(IncludeParams);
	const FCortexCommandResult Second = ListGraphs(IncludeParams);
	TestTrue(TEXT("include_subgraphs succeeds"), First.bSuccess);
	TestEqual(TEXT("identical output on a second call"), TreeToCondensedString(Second.Data), TreeToCondensedString(First.Data));
	TestFalse(TEXT("an untruncated tree reports no truncation"), First.Data.IsValid() && First.Data->HasField(TEXT("truncated")));

	const TArray<TSharedPtr<FJsonValue>> All = GraphsOf(First);
	// EventGraph, UserConstructionScript, HostFn, then HostFn's nested graphs: Transition, Grandchild, Transition~2, Inner (composite), SubOnly.
	TestEqual(TEXT("three roots plus five nested graphs (the cycle and the null entry add nothing)"), All.Num(), 8);

	const TSharedPtr<FJsonObject> T1 = FindTreeJsonObject(All, TEXT("path"), TEXT("HostFn/Transition"));
	const TSharedPtr<FJsonObject> T2 = FindTreeJsonObject(All, TEXT("path"), TEXT("HostFn/Transition~2"));
	const TSharedPtr<FJsonObject> Grandchild = FindTreeJsonObject(All, TEXT("path"), TEXT("HostFn/Transition/Grandchild"));
	const TSharedPtr<FJsonObject> Inner = FindTreeJsonObject(All, TEXT("path"), TEXT("HostFn/Inner"));
	const TSharedPtr<FJsonObject> SubOnly = FindTreeJsonObject(All, TEXT("path"), TEXT("HostFn/SubOnly"));
	TestTrue(TEXT("first Transition listed under a path"), T1.IsValid());
	TestTrue(TEXT("second Transition gets a distinct path"), T2.IsValid());
	TestTrue(TEXT("grandchild listed below its parent"), Grandchild.IsValid());
	TestTrue(TEXT("composite body listed"), Inner.IsValid());
	TestTrue(TEXT("SubGraphs-only graph listed"), SubOnly.IsValid());

	if (T1.IsValid() && T2.IsValid() && Grandchild.IsValid() && Inner.IsValid() && SubOnly.IsValid())
	{
		TestEqual(TEXT("T1 kind"), T1->GetStringField(TEXT("kind")), FString(TEXT("nested")));
		TestEqual(TEXT("T1 graph_guid"), T1->GetStringField(TEXT("graph_guid")), TreeGuidString(Fixture.T1));
		TestEqual(TEXT("T1 parent_graph_guid"), T1->GetStringField(TEXT("parent_graph_guid")), TreeGuidString(Fixture.HostFn));
		TestEqual(TEXT("T1 owner_node_guid"), T1->GetStringField(TEXT("owner_node_guid")), Fixture.HostNode->NodeGuid.ToString());
		TestEqual(TEXT("T2 kind"), T2->GetStringField(TEXT("kind")), FString(TEXT("nested")));
		TestEqual(TEXT("T2 graph_guid"), T2->GetStringField(TEXT("graph_guid")), TreeGuidString(Fixture.T2));
		TestEqual(TEXT("T2 owner_node_guid"), T2->GetStringField(TEXT("owner_node_guid")), Fixture.HostNode->NodeGuid.ToString());
		TestEqual(TEXT("both Transitions keep their name"), T2->GetStringField(TEXT("name")), T1->GetStringField(TEXT("name")));
		TestTrue(TEXT("distinct GUIDs are unique"), T1->GetBoolField(TEXT("graph_guid_unique")) && T2->GetBoolField(TEXT("graph_guid_unique")));

		TestEqual(TEXT("grandchild kind"), Grandchild->GetStringField(TEXT("kind")), FString(TEXT("nested")));
		TestEqual(TEXT("grandchild parent is T1"), Grandchild->GetStringField(TEXT("parent_graph_guid")), TreeGuidString(Fixture.T1));
		TestEqual(TEXT("grandchild owner is the inner host"), Grandchild->GetStringField(TEXT("owner_node_guid")), Fixture.InnerHost->NodeGuid.ToString());

		TestEqual(TEXT("composite kind"), Inner->GetStringField(TEXT("kind")), FString(TEXT("composite")));
		TestEqual(TEXT("composite keeps its subgraph_path"), Inner->GetStringField(TEXT("subgraph_path")), FString(TEXT("Inner")));
		TestEqual(TEXT("composite keeps parent_graph"), Inner->GetStringField(TEXT("parent_graph")), FString(TEXT("HostFn")));
		TestEqual(TEXT("composite owner is the composite node"), Inner->GetStringField(TEXT("owner_node_guid")), Fixture.Composite->NodeGuid.ToString());

		TestEqual(TEXT("SubGraphs-only kind"), SubOnly->GetStringField(TEXT("kind")), FString(TEXT("nested")));
		TestFalse(TEXT("SubGraphs-only graph has no owner node"), SubOnly->HasField(TEXT("owner_node_guid")));
		TestEqual(TEXT("SubGraphs-only parent"), SubOnly->GetStringField(TEXT("parent_graph_guid")), TreeGuidString(Fixture.HostFn));
	}

	// ── kind and parent_graph_guid filters narrow ──
	{
		TSharedPtr<FJsonObject> Params = MakeTreeParams(AssetPath);
		Params->SetBoolField(TEXT("include_subgraphs"), true);
		Params->SetStringField(TEXT("kind"), TEXT("nested"));
		const TArray<TSharedPtr<FJsonValue>> Nested = GraphsOf(ListGraphs(Params));
		TestEqual(TEXT("kind:nested lists T1, T2, Grandchild and SubOnly"), Nested.Num(), 4);
		for (const TSharedPtr<FJsonValue>& Item : Nested)
		{
			TestEqual(TEXT("every filtered entry is nested"), Item->AsObject()->GetStringField(TEXT("kind")), FString(TEXT("nested")));
		}

		Params = MakeTreeParams(AssetPath);
		Params->SetBoolField(TEXT("include_subgraphs"), true);
		Params->SetStringField(TEXT("parent_graph_guid"), TreeGuidString(Fixture.T1));
		const TArray<TSharedPtr<FJsonValue>> Children = GraphsOf(ListGraphs(Params));
		TestEqual(TEXT("parent_graph_guid narrows to the one child of T1"), Children.Num(), 1);
		if (Children.Num() == 1)
		{
			TestEqual(TEXT("the child is the grandchild"), Children[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Grandchild")));
		}

		Params = MakeTreeParams(AssetPath);
		Params->SetBoolField(TEXT("include_subgraphs"), true);
		Params->SetStringField(TEXT("kind"), TEXT("composite"));
		TestEqual(TEXT("kind:composite lists the composite body"), GraphsOf(ListGraphs(Params)).Num(), 1);

		Params = MakeTreeParams(AssetPath);
		Params->SetStringField(TEXT("parent_graph_guid"), TEXT("not-a-guid"));
		const FCortexCommandResult Bad = ListGraphs(Params);
		TestFalse(TEXT("an unparsable parent_graph_guid is refused"), Bad.bSuccess);
		TestEqual(TEXT("refusal code"), Bad.ErrorCode, CortexErrorCodes::InvalidField);
	}
	Fixture.Blueprint->MarkAsGarbage();

	// ── Depth limit: a tree deeper than 8 reports the cut instead of dropping silently ──
	{
		UBlueprint* DeepBP = CreateTreeBlueprint(TEXT("BP_GraphTreeDeep"));
		if (!TestNotNull(TEXT("deep Blueprint"), DeepBP))
		{
			return false;
		}
		UEdGraph* DeepRoot = AddTreeFunctionGraph(DeepBP, TEXT("DeepFn"));
		UEdGraph* Parent = DeepRoot;
		TArray<UCortexGraphTestNestedGraph*> Chain;
		for (int32 Level = 1; Level <= 10; ++Level)
		{
			UCortexGraphTestNestedGraph* Child = NewTreeNestedGraph(Parent, *FString::Printf(TEXT("Level%d"), Level));
			AddTreeHostNode(Parent, { Child });
			Chain.Add(Child);
			Parent = Child;
		}

		TSharedPtr<FJsonObject> Params = MakeTreeParams(DeepBP->GetPathName());
		Params->SetBoolField(TEXT("include_subgraphs"), true);
		const FCortexCommandResult Result = ListGraphs(Params);
		TestTrue(TEXT("deep list succeeds"), Result.bSuccess);
		const TArray<TSharedPtr<FJsonValue>> Graphs = GraphsOf(Result);
		// EventGraph + UserConstructionScript + DeepFn + Level1..Level8
		TestEqual(TEXT("levels 1..8 are listed, deeper levels are cut"), Graphs.Num(), 11);
		TestTrue(TEXT("Level8 listed"), FindTreeJsonObject(Graphs, TEXT("name"), TEXT("Level8")).IsValid());
		TestFalse(TEXT("Level9 is not listed"), FindTreeJsonObject(Graphs, TEXT("name"), TEXT("Level9")).IsValid());

		bool bTruncated = false;
		TestTrue(TEXT("truncated field present"), Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("truncated"), bTruncated));
		TestTrue(TEXT("truncated is true"), bTruncated);
		const TArray<TSharedPtr<FJsonValue>>* CutParents = nullptr;
		TestTrue(TEXT("truncated_parent_graph_guids present"),
			Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("truncated_parent_graph_guids"), CutParents) && CutParents);
		if (CutParents && TestEqual(TEXT("exactly one cut parent"), CutParents->Num(), 1))
		{
			TestEqual(TEXT("the cut parent is Level8"), (*CutParents)[0]->AsString(), TreeGuidString(Chain[7]));
		}
		DeepBP->MarkAsGarbage();
	}

	// ── Two nested graphs sharing one GraphGuid are both flagged ──
	{
		FNestedFixture Shared;
		if (!TestTrue(TEXT("shared-guid fixture builds"), BuildTreeNestedFixture(TEXT("BP_GraphTreeSharedGuid"), Shared)))
		{
			return false;
		}
		Shared.T2->GraphGuid = Shared.T1->GraphGuid;

		TSharedPtr<FJsonObject> Params = MakeTreeParams(Shared.Blueprint->GetPathName());
		Params->SetBoolField(TEXT("include_subgraphs"), true);
		const TArray<TSharedPtr<FJsonValue>> Graphs = GraphsOf(ListGraphs(Params));
		const TSharedPtr<FJsonObject> Left = FindTreeJsonObject(Graphs, TEXT("path"), TEXT("HostFn/Transition"));
		const TSharedPtr<FJsonObject> Right = FindTreeJsonObject(Graphs, TEXT("path"), TEXT("HostFn/Transition~2"));
		if (TestTrue(TEXT("both colliding graphs listed"), Left.IsValid() && Right.IsValid()))
		{
			TestFalse(TEXT("first colliding graph flagged"), Left->GetBoolField(TEXT("graph_guid_unique")));
			TestFalse(TEXT("second colliding graph flagged"), Right->GetBoolField(TEXT("graph_guid_unique")));
			TestEqual(TEXT("they really share the GUID"), Left->GetStringField(TEXT("graph_guid")), Right->GetStringField(TEXT("graph_guid")));
		}
		const TSharedPtr<FJsonObject> Other = FindTreeJsonObject(Graphs, TEXT("path"), TEXT("HostFn/SubOnly"));
		if (TestTrue(TEXT("a non-colliding graph is listed"), Other.IsValid()))
		{
			TestTrue(TEXT("non-colliding graph stays unique"), Other->GetBoolField(TEXT("graph_guid_unique")));
		}
		Shared.Blueprint->MarkAsGarbage();
	}

	return true;
}
