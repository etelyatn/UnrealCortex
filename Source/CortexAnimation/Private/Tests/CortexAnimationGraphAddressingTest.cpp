#include "Misc/AutomationTest.h"

#include "CortexAnimationTestAnimBlueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationGraphAddressingTest,
	"Cortex.Animation.GraphAddressing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationGraphAddressingTest::RunTest(const FString& Parameters)
{
	using namespace CortexAnimTest;

	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);

	// Two machines, three states (Idle, Walk in Locomotion; Aim in Upper), two transitions that are both named Transition,
	// one conduit, and a custom blend graph on the first transition.
	UAnimGraphNode_StateMachine* Locomotion = AddStateMachine(AnimBlueprint, TEXT("Locomotion"));
	UAnimGraphNode_StateMachine* Upper = AddStateMachine(AnimBlueprint, TEXT("Upper"));
	if (!TestTrue(TEXT("State machines are created"), Locomotion != nullptr && Upper != nullptr))
	{
		return false;
	}
	UAnimStateNode* Idle = AddState(Locomotion, TEXT("Idle"));
	UAnimStateNode* Walk = AddState(Locomotion, TEXT("Walk"));
	UAnimStateNode* Aim = AddState(Upper, TEXT("Aim"));
	UAnimStateConduitNode* Gate = AddConduit(Locomotion, TEXT("Gate"));
	if (!TestTrue(TEXT("States and conduit are created"), Idle != nullptr && Walk != nullptr && Aim != nullptr && Gate != nullptr))
	{
		return false;
	}
	UAnimStateTransitionNode* IdleToWalk = AddTransition(Locomotion, Idle, Walk);
	UAnimStateTransitionNode* WalkToIdle = AddTransition(Locomotion, Walk, Idle);
	if (!TestTrue(TEXT("Transitions are created"), IdleToWalk != nullptr && WalkToIdle != nullptr
		&& IdleToWalk->BoundGraph != nullptr && WalkToIdle->BoundGraph != nullptr))
	{
		return false;
	}
	UEdGraph* CustomBlend = AddCustomTransitionGraph(IdleToWalk);
	TestNotNull(TEXT("Custom transition graph"), CustomBlend);
	TestEqual(TEXT("Both transition rule graphs are named Transition"), IdleToWalk->BoundGraph->GetName() + WalkToIdle->BoundGraph->GetName(), FString(TEXT("TransitionTransition")));

	UAnimationGraph* AnimGraph = FindRootAnimGraph(AnimBlueprint);
	if (!TestNotNull(TEXT("Root AnimGraph"), AnimGraph))
	{
		return false;
	}

	FCortexCommandRouter Router = MakeAnimGraphRouter();

	// ── graph.list_graphs: the whole anim tree, kinds from the owner node class ──
	TSharedPtr<FJsonObject> ListParams = MakeAssetParams(AnimBlueprint);
	ListParams->SetBoolField(TEXT("include_subgraphs"), true);
	const FCortexCommandResult List = Router.Execute(TEXT("graph.list_graphs"), ListParams);
	if (!TestTrue(TEXT("list_graphs succeeds"), List.bSuccess) || !TestTrue(TEXT("list_graphs data"), List.Data.IsValid()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> Graphs = JsonObjectArray(List.Data, TEXT("graphs"));

	TMap<FString, int32> KindCounts;
	for (const TSharedPtr<FJsonObject>& Entry : Graphs)
	{
		KindCounts.FindOrAdd(Entry->GetStringField(TEXT("kind")))++;
		TestTrue(FString::Printf(TEXT("%s carries a graph_guid"), *Entry->GetStringField(TEXT("name"))), FGuid(Entry->GetStringField(TEXT("graph_guid"))).IsValid());
	}
	TestEqual(TEXT("anim_state_machine entries"), KindCounts.FindRef(TEXT("anim_state_machine")), 2);
	TestEqual(TEXT("anim_state entries"), KindCounts.FindRef(TEXT("anim_state")), 3);
	TestEqual(TEXT("anim_transition_rule entries"), KindCounts.FindRef(TEXT("anim_transition_rule")), 2);
	TestEqual(TEXT("exactly one anim_conduit"), KindCounts.FindRef(TEXT("anim_conduit")), 1);
	TestEqual(TEXT("the custom blend graph is not a transition rule"), KindCounts.FindRef(TEXT("anim_custom_transition")), 1);

	const TSharedPtr<FJsonObject> Root = FindJsonObjectByGuid(Graphs, TEXT("graph_guid"), AnimGraph->GraphGuid);
	if (TestTrue(TEXT("The root AnimGraph is listed"), Root.IsValid()))
	{
		TestEqual(TEXT("the root AnimGraph keeps its root kind"), Root->GetStringField(TEXT("kind")), FString(TEXT("function")));
		TestEqual(TEXT("graph_class_kind marks the AnimGraph"), Root->GetStringField(TEXT("graph_class_kind")), FString(TEXT("anim_graph")));
	}

	const TSharedPtr<FJsonObject> MachineEntry = FindJsonObjectByGuid(Graphs, TEXT("graph_guid"), Locomotion->EditorStateMachineGraph->GraphGuid);
	const TSharedPtr<FJsonObject> IdleEntry = FindJsonObjectByGuid(Graphs, TEXT("graph_guid"), Idle->BoundGraph->GraphGuid);
	const TSharedPtr<FJsonObject> RuleEntry = FindJsonObjectByGuid(Graphs, TEXT("graph_guid"), IdleToWalk->BoundGraph->GraphGuid);
	const TSharedPtr<FJsonObject> ConduitEntry = FindJsonObjectByGuid(Graphs, TEXT("graph_guid"), Gate->BoundGraph->GraphGuid);
	if (TestTrue(TEXT("machine, state, rule and conduit graphs are all listed"), MachineEntry.IsValid() && IdleEntry.IsValid() && RuleEntry.IsValid() && ConduitEntry.IsValid()))
	{
		TestEqual(TEXT("machine parent is the AnimGraph"), MachineEntry->GetStringField(TEXT("parent_graph_guid")), GuidString(AnimGraph->GraphGuid));
		TestEqual(TEXT("state parent is the machine"), IdleEntry->GetStringField(TEXT("parent_graph_guid")), GuidString(Locomotion->EditorStateMachineGraph->GraphGuid));
		TestEqual(TEXT("rule parent is the machine"), RuleEntry->GetStringField(TEXT("parent_graph_guid")), GuidString(Locomotion->EditorStateMachineGraph->GraphGuid));
		TestEqual(TEXT("conduit kind comes from the owner node"), ConduitEntry->GetStringField(TEXT("kind")), FString(TEXT("anim_conduit")));
		TestEqual(TEXT("rule kind"), RuleEntry->GetStringField(TEXT("kind")), FString(TEXT("anim_transition_rule")));
		TestEqual(TEXT("conduit parent is the machine"), ConduitEntry->GetStringField(TEXT("parent_graph_guid")), GuidString(Locomotion->EditorStateMachineGraph->GraphGuid));
	}

	// ── graph.get_subgraph by graph_guid: state, transition rule, conduit ──
	auto SubgraphClasses = [&](const FGuid& GraphGuid, FString& OutKind) -> TSet<FString>
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
		Params->SetStringField(TEXT("graph_guid"), GuidString(GraphGuid));
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.get_subgraph"), Params);
		TSet<FString> Classes;
		if (TestTrue(*FString::Printf(TEXT("get_subgraph %s succeeds: %s"), *GuidString(GraphGuid), *Result.ErrorMessage), Result.bSuccess) && Result.Data.IsValid())
		{
			OutKind = Result.Data->GetStringField(TEXT("graph_kind"));
			for (const TSharedPtr<FJsonObject>& Node : JsonObjectArray(Result.Data, TEXT("nodes")))
			{
				Classes.Add(Node->GetStringField(TEXT("class")));
			}
		}
		return Classes;
	};
	{
		FString Kind;
		const TSet<FString> StateNodes = SubgraphClasses(Idle->BoundGraph->GraphGuid, Kind);
		TestTrue(TEXT("the state graph has its result node"), StateNodes.Contains(TEXT("AnimGraphNode_StateResult")));
		TestEqual(TEXT("state graph kind"), Kind, FString(TEXT("anim_state")));
	}
	{
		FString Kind;
		const TSet<FString> RuleNodes = SubgraphClasses(IdleToWalk->BoundGraph->GraphGuid, Kind);
		TestTrue(TEXT("the transition rule graph has its result node"), RuleNodes.Contains(TEXT("AnimGraphNode_TransitionResult")));
		TestEqual(TEXT("transition rule graph kind"), Kind, FString(TEXT("anim_transition_rule")));
	}
	{
		FString Kind;
		const TSet<FString> ConduitNodes = SubgraphClasses(Gate->BoundGraph->GraphGuid, Kind);
		TestTrue(TEXT("the conduit graph has its result node"), ConduitNodes.Contains(TEXT("AnimGraphNode_TransitionResult")));
		TestEqual(TEXT("conduit graph kind"), Kind, FString(TEXT("anim_conduit")));
	}

	// ── The two Transition graphs are only reachable by graph_guid ──
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
		Params->SetStringField(TEXT("graph_name"), TEXT("Transition"));
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.get_subgraph"), Params);
		TestFalse(TEXT("a nested-only name is not resolved"), Result.bSuccess);
		TestEqual(TEXT("GRAPH_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::GraphNotFound);
		const TArray<TSharedPtr<FJsonObject>> Candidates = Result.ErrorDetails.IsValid()
			? JsonObjectArray(Result.ErrorDetails, TEXT("nested_candidates")) : TArray<TSharedPtr<FJsonObject>>();
		TestEqual(TEXT("both Transition graphs are offered as nested_candidates"), Candidates.Num(), 2);
		TestTrue(TEXT("first candidate by guid"), FindJsonObjectByGuid(Candidates, TEXT("graph_guid"), IdleToWalk->BoundGraph->GraphGuid).IsValid());
		TestTrue(TEXT("second candidate by guid"), FindJsonObjectByGuid(Candidates, TEXT("graph_guid"), WalkToIdle->BoundGraph->GraphGuid).IsValid());
	}

	// ── anim.get_animbp_info: conduits and the custom transition graph, guid-equal to graph.list_graphs ──
	const FCortexCommandResult Info = Router.Execute(TEXT("anim.get_animbp_info"), MakeAssetParams(AnimBlueprint));
	if (TestTrue(TEXT("get_animbp_info succeeds"), Info.bSuccess) && TestTrue(TEXT("get_animbp_info data"), Info.Data.IsValid()))
	{
		const TArray<TSharedPtr<FJsonObject>> Machines = CollectionItems(Info.Data, TEXT("state_machines"));
		const TSharedPtr<FJsonObject> Machine = FindJsonObject(Machines, TEXT("name"), TEXT("Locomotion"));
		if (TestTrue(TEXT("Locomotion is reported"), Machine.IsValid()))
		{
			const TArray<TSharedPtr<FJsonObject>> Conduits = CollectionItems(Machine, TEXT("conduits"));
			if (TestEqual(TEXT("one conduit is reported"), Conduits.Num(), 1))
			{
				TestEqual(TEXT("conduit name"), Conduits[0]->GetStringField(TEXT("name")), FString(TEXT("Gate")));
				TestTrue(TEXT("conduit node_guid"), SameGuid(Conduits[0]->GetStringField(TEXT("node_guid")), Gate->NodeGuid));
				TestTrue(TEXT("conduit graph_guid equals list_graphs"), SameGuid(Conduits[0]->GetStringField(TEXT("graph_guid")), Gate->BoundGraph->GraphGuid));
			}

			const TArray<TSharedPtr<FJsonObject>> Transitions = CollectionItems(Machine, TEXT("transitions"));
			const TSharedPtr<FJsonObject> First = FindJsonObjectByGuid(Transitions, TEXT("node_guid"), IdleToWalk->NodeGuid);
			const TSharedPtr<FJsonObject> Second = FindJsonObjectByGuid(Transitions, TEXT("node_guid"), WalkToIdle->NodeGuid);
			if (TestTrue(TEXT("both transitions are reported"), First.IsValid() && Second.IsValid()))
			{
				TestTrue(TEXT("custom_transition_graph_guid names the custom blend graph"), CustomBlend != nullptr && SameGuid(First->GetStringField(TEXT("custom_transition_graph_guid")), CustomBlend->GraphGuid));
				TestEqual(TEXT("a transition without a custom graph reports an empty guid"), Second->GetStringField(TEXT("custom_transition_graph_guid")), FString());
				TestTrue(TEXT("transition graph_guid equals list_graphs"), SameGuid(First->GetStringField(TEXT("graph_guid")), IdleToWalk->BoundGraph->GraphGuid));
			}
		}

		// conduit_limit bounds the list per machine and reports the cut.
		TestNotNull(TEXT("A second conduit"), AddConduit(Locomotion, TEXT("Gate2")));
		TSharedPtr<FJsonObject> Limited = MakeAssetParams(AnimBlueprint);
		Limited->SetNumberField(TEXT("conduit_limit"), 1);
		const FCortexCommandResult LimitedInfo = Router.Execute(TEXT("anim.get_animbp_info"), Limited);
		if (TestTrue(TEXT("get_animbp_info with conduit_limit succeeds"), LimitedInfo.bSuccess))
		{
			const TSharedPtr<FJsonObject> LimitedMachine = FindJsonObject(CollectionItems(LimitedInfo.Data, TEXT("state_machines")), TEXT("name"), TEXT("Locomotion"));
			const TSharedPtr<FJsonObject>* Collection = nullptr;
			if (LimitedMachine.IsValid() && TestTrue(TEXT("conduits collection exists"), LimitedMachine->TryGetObjectField(TEXT("conduits"), Collection)))
			{
				TestEqual(TEXT("conduit count"), static_cast<int32>((*Collection)->GetNumberField(TEXT("count"))), 2);
				TestEqual(TEXT("conduit returned"), static_cast<int32>((*Collection)->GetNumberField(TEXT("returned"))), 1);
				TestTrue(TEXT("conduit truncated"), (*Collection)->GetBoolField(TEXT("truncated")));
			}
		}
	}

	return true;
}
