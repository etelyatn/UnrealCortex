#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphNodeOps.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_Composite.h"
#include "K2Node_Tunnel.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"

// ── Shared test helper ──────────────────────────────────────────────────
namespace CortexSubgraphTestUtils
{
	/** Create a transient Blueprint and return it + its EventGraph. */
	inline UBlueprint* CreateTestBP(const TCHAR* Name, UEdGraph*& OutEventGraph)
	{
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			GetTransientPackage(),
			FName(Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass()
		);
		OutEventGraph = nullptr;
		if (BP)
		{
			for (UEdGraph* G : BP->UbergraphPages)
			{
				if (G && G->GetName() == TEXT("EventGraph"))
				{
					OutEventGraph = G;
					break;
				}
			}
		}
		return BP;
	}

	/** Create a composite node with a named BoundGraph inside ParentGraph. */
	inline UK2Node_Composite* CreateComposite(UBlueprint* BP, UEdGraph* ParentGraph, const TCHAR* SubgraphName)
	{
		UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(ParentGraph);
		Composite->CreateNewGuid();
		ParentGraph->AddNode(Composite, true, false);

		UEdGraph* Sub = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(SubgraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass()
		);
		Composite->BoundGraph = Sub;
		Composite->AllocateDefaultPins();
		return Composite;
	}
}

// ── Test 1: Basic resolve ───────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSubgraphResolveTest,
	"Cortex.Graph.Subgraph.Resolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSubgraphResolveTest::RunTest(const FString& Parameters)
{
	using namespace CortexSubgraphTestUtils;

	UEdGraph* EventGraph = nullptr;
	UBlueprint* TestBP = CreateTestBP(TEXT("BP_SubgraphResolveTest"), EventGraph);
	TestNotNull(TEXT("TestBP created"), TestBP);
	TestNotNull(TEXT("EventGraph found"), EventGraph);

	CreateComposite(TestBP, EventGraph, TEXT("MyComposite"));

	// Valid single-segment path
	FCortexCommandResult Error;
	UEdGraph* Resolved = FCortexGraphNodeOps::ResolveSubgraph(EventGraph, TEXT("MyComposite"), Error);
	TestNotNull(TEXT("ResolveSubgraph should find MyComposite"), Resolved);
	if (Resolved)
	{
		TestEqual(TEXT("Resolved graph name matches"), Resolved->GetName(), FString(TEXT("MyComposite")));
	}

	// Empty path returns root graph
	UEdGraph* Root = FCortexGraphNodeOps::ResolveSubgraph(EventGraph, TEXT(""), Error);
	TestEqual(TEXT("Empty subgraph_path returns root graph"), Root, EventGraph);

	// Invalid path returns null
	UEdGraph* Invalid = FCortexGraphNodeOps::ResolveSubgraph(EventGraph, TEXT("NonExistent"), Error);
	TestNull(TEXT("Invalid subgraph_path returns null"), Invalid);
	TestEqual(TEXT("Error code is SUBGRAPH_NOT_FOUND"), Error.ErrorCode, CortexErrorCodes::SubgraphNotFound);

	TestBP->MarkAsGarbage();
	return true;
}

// ── Test 2: Depth limit ─────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSubgraphDepthTest,
	"Cortex.Graph.Subgraph.DepthLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSubgraphDepthTest::RunTest(const FString& Parameters)
{
	using namespace CortexSubgraphTestUtils;

	UEdGraph* EventGraph = nullptr;
	UBlueprint* TestBP = CreateTestBP(TEXT("BP_SubgraphDepthTest"), EventGraph);
	TestNotNull(TEXT("TestBP created"), TestBP);
	TestNotNull(TEXT("EventGraph found"), EventGraph);

	// Create a chain of 6 nested composites (exceeds limit of 5)
	UEdGraph* CurrentGraph = EventGraph;
	for (int32 i = 0; i < 6; ++i)
	{
		FString SubName = FString::Printf(TEXT("Level%d"), i);
		UK2Node_Composite* Composite = CreateComposite(TestBP, CurrentGraph, *SubName);
		CurrentGraph = Composite->BoundGraph;
	}

	// 5-deep path should succeed
	FCortexCommandResult Error;
	UEdGraph* FiveDeep = FCortexGraphNodeOps::ResolveSubgraph(
		EventGraph, TEXT("Level0.Level1.Level2.Level3.Level4"), Error);
	TestNotNull(TEXT("5-deep path resolves"), FiveDeep);

	// 6-deep path should fail with depth exceeded
	UEdGraph* SixDeep = FCortexGraphNodeOps::ResolveSubgraph(
		EventGraph, TEXT("Level0.Level1.Level2.Level3.Level4.Level5"), Error);
	TestNull(TEXT("6-deep path returns null"), SixDeep);
	TestEqual(TEXT("Error code is SUBGRAPH_DEPTH_EXCEEDED"), Error.ErrorCode, CortexErrorCodes::SubgraphDepthExceeded);

	// 2-deep nested path works
	UEdGraph* TwoDeep = FCortexGraphNodeOps::ResolveSubgraph(
		EventGraph, TEXT("Level0.Level1"), Error);
	TestNotNull(TEXT("2-deep nested path resolves"), TwoDeep);
	if (TwoDeep)
	{
		TestEqual(TEXT("Resolved graph is Level1"), TwoDeep->GetName(), FString(TEXT("Level1")));
	}

	TestBP->MarkAsGarbage();
	return true;
}

// ── Test 3: Function graph composite ───────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSubgraphFunctionGraphTest,
	"Cortex.Graph.Subgraph.FunctionGraphComposite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSubgraphFunctionGraphTest::RunTest(const FString& Parameters)
{
	using namespace CortexSubgraphTestUtils;

	UEdGraph* EventGraph = nullptr;
	UBlueprint* TestBP = CreateTestBP(TEXT("BP_SubgraphFuncGraphTest"), EventGraph);
	TestNotNull(TEXT("TestBP created"), TestBP);

	// Add a function graph
	UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
		TestBP,
		FName(TEXT("MyFunction")),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);
	TestBP->FunctionGraphs.Add(FuncGraph);

	// Create a composite inside the function graph
	CreateComposite(TestBP, FuncGraph, TEXT("FuncComposite"));

	// Resolve via FindGraph + ResolveSubgraph (mimics how commands work)
	FCortexCommandResult Error;
	UEdGraph* FoundFunc = FCortexGraphNodeOps::FindGraph(TestBP, TEXT("MyFunction"), Error);
	TestNotNull(TEXT("FindGraph finds MyFunction"), FoundFunc);

	UEdGraph* Resolved = FCortexGraphNodeOps::ResolveSubgraph(FoundFunc, TEXT("FuncComposite"), Error);
	TestNotNull(TEXT("Composite inside function graph resolves"), Resolved);
	if (Resolved)
	{
		TestEqual(TEXT("Resolved graph name"), Resolved->GetName(), FString(TEXT("FuncComposite")));
	}

	TestBP->MarkAsGarbage();
	return true;
}

// ── Test 4: Discovery ───────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSubgraphDiscoveryTest,
	"Cortex.Graph.Subgraph.Discovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSubgraphDiscoveryTest::RunTest(const FString& Parameters)
{
	using namespace CortexSubgraphTestUtils;

	UEdGraph* EventGraph = nullptr;
	UBlueprint* TestBP = CreateTestBP(TEXT("BP_SubgraphDiscoveryTest"), EventGraph);
	TestNotNull(TEXT("TestBP created"), TestBP);
	TestNotNull(TEXT("EventGraph found"), EventGraph);

	CreateComposite(TestBP, EventGraph, TEXT("BeginPlay"));

	// Use command router to call list_nodes
	FCortexCommandRouter Router;
	Router.RegisterDomain(
		TEXT("graph"), TEXT("Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>()
	);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));

	FCortexCommandResult Result = Router.Execute(TEXT("graph.list_nodes"), Params);
	TestTrue(TEXT("list_nodes succeeds"), Result.bSuccess);

	// Find the composite node entry and check subgraph_name
	const TArray<TSharedPtr<FJsonValue>>* Nodes;
	if (Result.Data->TryGetArrayField(TEXT("nodes"), Nodes))
	{
		bool bFoundComposite = false;
		for (const auto& NodeVal : *Nodes)
		{
			const TSharedPtr<FJsonObject>& NodeObj = NodeVal->AsObject();
			FString ClassName;
			NodeObj->TryGetStringField(TEXT("class"), ClassName);
			if (ClassName == TEXT("K2Node_Composite"))
			{
				bFoundComposite = true;
				FString SubgraphName;
				TestTrue(TEXT("Composite node has subgraph_name field"),
					NodeObj->TryGetStringField(TEXT("subgraph_name"), SubgraphName));
				TestEqual(TEXT("subgraph_name is BeginPlay"), SubgraphName, FString(TEXT("BeginPlay")));
			}
		}
		TestTrue(TEXT("Found composite node in list"), bFoundComposite);
	}

	// Now list nodes INSIDE the subgraph and verify tunnel boundaries are annotated
	TSharedPtr<FJsonObject> SubParams = MakeShared<FJsonObject>();
	SubParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	SubParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	SubParams->SetStringField(TEXT("subgraph_path"), TEXT("BeginPlay"));

	FCortexCommandResult SubResult = Router.Execute(TEXT("graph.list_nodes"), SubParams);
	TestTrue(TEXT("list_nodes in subgraph succeeds"), SubResult.bSuccess);

	// The BoundGraph may contain tunnel entry/exit nodes from AllocateDefaultPins
	// Verify they are annotated with is_tunnel_boundary
	const TArray<TSharedPtr<FJsonValue>>* SubNodes;
	if (SubResult.Data->TryGetArrayField(TEXT("nodes"), SubNodes))
	{
		for (const auto& NodeVal : *SubNodes)
		{
			const TSharedPtr<FJsonObject>& NodeObj = NodeVal->AsObject();
			FString ClassName;
			NodeObj->TryGetStringField(TEXT("class"), ClassName);
			if (ClassName.Contains(TEXT("Tunnel")))
			{
				bool bIsTunnelBoundary = false;
				NodeObj->TryGetBoolField(TEXT("is_tunnel_boundary"), bIsTunnelBoundary);
				TestTrue(TEXT("Tunnel node has is_tunnel_boundary=true"), bIsTunnelBoundary);
			}
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}

// ── Test 5: ListNodes with subgraph_path ────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSubgraphListNodesTest,
	"Cortex.Graph.Subgraph.ListNodesInSubgraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSubgraphListNodesTest::RunTest(const FString& Parameters)
{
	using namespace CortexSubgraphTestUtils;

	UEdGraph* EventGraph = nullptr;
	UBlueprint* TestBP = CreateTestBP(TEXT("BP_SubgraphListNodesTest"), EventGraph);
	TestNotNull(TEXT("TestBP created"), TestBP);
	TestNotNull(TEXT("EventGraph found"), EventGraph);

	UK2Node_Composite* Composite = CreateComposite(TestBP, EventGraph, TEXT("InnerGraph"));
	UEdGraph* SubGraph = Composite->BoundGraph;

	// Add a node inside the subgraph
	UK2Node_CallFunction* InnerNode = NewObject<UK2Node_CallFunction>(SubGraph);
	InnerNode->CreateNewGuid();
	InnerNode->FunctionReference.SetExternalMember(
		FName(TEXT("PrintString")),
		UKismetSystemLibrary::StaticClass()
	);
	SubGraph->AddNode(InnerNode, true, false);
	InnerNode->AllocateDefaultPins();

	// Call list_nodes with subgraph_path
	FCortexCommandRouter Router;
	Router.RegisterDomain(
		TEXT("graph"), TEXT("Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>()
	);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Params->SetStringField(TEXT("subgraph_path"), TEXT("InnerGraph"));

	FCortexCommandResult Result = Router.Execute(TEXT("graph.list_nodes"), Params);
	TestTrue(TEXT("list_nodes with subgraph_path succeeds"), Result.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Nodes;
	if (Result.Data->TryGetArrayField(TEXT("nodes"), Nodes))
	{
		TestTrue(TEXT("Subgraph has at least 1 node"), Nodes->Num() >= 1);
		bool bFoundCallFunction = false;
		for (const auto& NodeVal : *Nodes)
		{
			FString ClassName;
			NodeVal->AsObject()->TryGetStringField(TEXT("class"), ClassName);
			if (ClassName == TEXT("K2Node_CallFunction"))
			{
				bFoundCallFunction = true;
			}
		}
		TestTrue(TEXT("Found CallFunction node inside subgraph"), bFoundCallFunction);
	}

	TestBP->MarkAsGarbage();
	return true;
}

// ── Test 6: SearchNodes recursive descent ──────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSubgraphSearchRecursiveTest,
	"Cortex.Graph.Subgraph.SearchRecursive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSubgraphSearchRecursiveTest::RunTest(const FString& Parameters)
{
	using namespace CortexSubgraphTestUtils;

	UEdGraph* EventGraph = nullptr;
	UBlueprint* TestBP = CreateTestBP(TEXT("BP_SubgraphSearchTest"), EventGraph);
	TestNotNull(TEXT("TestBP created"), TestBP);
	TestNotNull(TEXT("EventGraph found"), EventGraph);

	// Create composite with a CallFunction node inside
	UK2Node_Composite* Composite = CreateComposite(TestBP, EventGraph, TEXT("SearchTarget"));
	UEdGraph* SubGraph = Composite->BoundGraph;

	UK2Node_CallFunction* InnerNode = NewObject<UK2Node_CallFunction>(SubGraph);
	InnerNode->CreateNewGuid();
	InnerNode->FunctionReference.SetExternalMember(
		FName(TEXT("PrintString")),
		UKismetSystemLibrary::StaticClass()
	);
	SubGraph->AddNode(InnerNode, true, false);
	InnerNode->AllocateDefaultPins();

	// search_nodes WITHOUT subgraph_path should still find the inner node
	FCortexCommandRouter Router;
	Router.RegisterDomain(
		TEXT("graph"), TEXT("Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>()
	);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("function_name"), TEXT("PrintString"));

	FCortexCommandResult Result = Router.Execute(TEXT("graph.search_nodes"), Params);
	TestTrue(TEXT("search_nodes succeeds"), Result.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Results;
	bool bFoundInSubgraph = false;
	if (Result.Data->TryGetArrayField(TEXT("results"), Results))
	{
		for (const auto& ResVal : *Results)
		{
			const TSharedPtr<FJsonObject>& ResObj = ResVal->AsObject();
			FString SubPath;
			ResObj->TryGetStringField(TEXT("subgraph_path"), SubPath);
			if (SubPath == TEXT("SearchTarget"))
			{
				bFoundInSubgraph = true;
			}
		}
	}
	TestTrue(TEXT("search_nodes found node inside composite (recursive)"), bFoundInSubgraph);

	TestBP->MarkAsGarbage();
	return true;
}

// ── Test 7: Write operations with subgraph_path ──────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSubgraphWriteTest,
	"Cortex.Graph.Subgraph.WriteOperations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSubgraphWriteTest::RunTest(const FString& Parameters)
{
	using namespace CortexSubgraphTestUtils;

	UEdGraph* EventGraph = nullptr;
	UBlueprint* TestBP = CreateTestBP(TEXT("BP_SubgraphWriteTest"), EventGraph);
	TestNotNull(TEXT("TestBP created"), TestBP);
	TestNotNull(TEXT("EventGraph found"), EventGraph);

	UK2Node_Composite* Composite = CreateComposite(TestBP, EventGraph, TEXT("WriteTarget"));
	UEdGraph* SubGraph = Composite->BoundGraph;
	int32 InitialNodeCount = SubGraph->Nodes.Num();

	FCortexCommandRouter Router;
	Router.RegisterDomain(
		TEXT("graph"), TEXT("Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>()
	);

	// add_node into subgraph
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	AddParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	AddParams->SetStringField(TEXT("subgraph_path"), TEXT("WriteTarget"));
	AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));

	FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
	TestTrue(TEXT("add_node in subgraph succeeds"), AddResult.bSuccess);
	TestEqual(TEXT("SubGraph has one more node"), SubGraph->Nodes.Num(), InitialNodeCount + 1);

	FString AddedNodeId;
	if (AddResult.Data)
	{
		AddResult.Data->TryGetStringField(TEXT("node_id"), AddedNodeId);
	}
	TestFalse(TEXT("Added node has an ID"), AddedNodeId.IsEmpty());

	// remove_node from subgraph
	TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
	RemoveParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	RemoveParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	RemoveParams->SetStringField(TEXT("subgraph_path"), TEXT("WriteTarget"));
	RemoveParams->SetStringField(TEXT("node_id"), AddedNodeId);

	FCortexCommandResult RemoveResult = Router.Execute(TEXT("graph.remove_node"), RemoveParams);
	TestTrue(TEXT("remove_node from subgraph succeeds"), RemoveResult.bSuccess);
	TestEqual(TEXT("SubGraph back to initial count"), SubGraph->Nodes.Num(), InitialNodeCount);

	TestBP->MarkAsGarbage();
	return true;
}

// ── Test 8: Connect and Disconnect with subgraph_path ───────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSubgraphConnectTest,
	"Cortex.Graph.Subgraph.Connect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSubgraphConnectTest::RunTest(const FString& Parameters)
{
	using namespace CortexSubgraphTestUtils;

	UEdGraph* EventGraph = nullptr;
	UBlueprint* TestBP = CreateTestBP(TEXT("BP_SubgraphConnectTest"), EventGraph);
	TestNotNull(TEXT("TestBP created"), TestBP);
	TestNotNull(TEXT("EventGraph found"), EventGraph);

	UK2Node_Composite* Composite = CreateComposite(TestBP, EventGraph, TEXT("ConnectTarget"));
	UEdGraph* SubGraph = Composite->BoundGraph;

	// Add two Branch nodes inside to connect exec pins
	UK2Node_IfThenElse* BranchA = NewObject<UK2Node_IfThenElse>(SubGraph);
	BranchA->CreateNewGuid();
	SubGraph->AddNode(BranchA, true, false);
	BranchA->AllocateDefaultPins();

	UK2Node_IfThenElse* BranchB = NewObject<UK2Node_IfThenElse>(SubGraph);
	BranchB->CreateNewGuid();
	SubGraph->AddNode(BranchB, true, false);
	BranchB->AllocateDefaultPins();

	FCortexCommandRouter Router;
	Router.RegisterDomain(
		TEXT("graph"), TEXT("Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>()
	);

	// Connect BranchA "Then" -> BranchB "Execute"
	TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
	ConnectParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	ConnectParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	ConnectParams->SetStringField(TEXT("subgraph_path"), TEXT("ConnectTarget"));
	ConnectParams->SetStringField(TEXT("source_node"), BranchA->GetName());
	ConnectParams->SetStringField(TEXT("source_pin"), TEXT("Then"));
	ConnectParams->SetStringField(TEXT("target_node"), BranchB->GetName());
	ConnectParams->SetStringField(TEXT("target_pin"), TEXT("execute"));

	FCortexCommandResult ConnectResult = Router.Execute(TEXT("graph.connect"), ConnectParams);
	TestTrue(TEXT("connect in subgraph succeeds"), ConnectResult.bSuccess);

	UEdGraphPin* ThenPin = BranchA->FindPin(TEXT("Then"));
	TestNotNull(TEXT("Then pin exists"), ThenPin);
	if (ThenPin)
	{
		TestTrue(TEXT("Then pin is connected"), ThenPin->LinkedTo.Num() > 0);
	}

	// Disconnect
	TSharedPtr<FJsonObject> DisconnectParams = MakeShared<FJsonObject>();
	DisconnectParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	DisconnectParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	DisconnectParams->SetStringField(TEXT("subgraph_path"), TEXT("ConnectTarget"));
	DisconnectParams->SetStringField(TEXT("node_id"), BranchA->GetName());
	DisconnectParams->SetStringField(TEXT("pin_name"), TEXT("Then"));

	FCortexCommandResult DisconnectResult = Router.Execute(TEXT("graph.disconnect"), DisconnectParams);
	TestTrue(TEXT("disconnect in subgraph succeeds"), DisconnectResult.bSuccess);

	if (ThenPin)
	{
		TestEqual(TEXT("Then pin is disconnected"), ThenPin->LinkedTo.Num(), 0);
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSubgraphListGraphsTest,
	"Cortex.Graph.Subgraph.ListGraphsWithSubgraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSubgraphListGraphsTest::RunTest(const FString& Parameters)
{
	using namespace CortexSubgraphTestUtils;

	UEdGraph* EventGraph = nullptr;
	UBlueprint* TestBP = CreateTestBP(TEXT("BP_SubgraphListGraphsTest"), EventGraph);
	TestNotNull(TEXT("TestBP created"), TestBP);
	TestNotNull(TEXT("EventGraph found"), EventGraph);

	CreateComposite(TestBP, EventGraph, TEXT("MyCollapsed"));

	FCortexCommandRouter Router;
	Router.RegisterDomain(
		TEXT("graph"), TEXT("Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>()
	);

	// Without include_subgraphs: only top-level graphs
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());

	FCortexCommandResult BasicResult = Router.Execute(TEXT("graph.list_graphs"), Params);
	TestTrue(TEXT("list_graphs succeeds"), BasicResult.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* BasicGraphs;
	bool bFoundSubgraphInBasic = false;
	if (BasicResult.Data->TryGetArrayField(TEXT("graphs"), BasicGraphs))
	{
		for (const auto& GVal : *BasicGraphs)
		{
			FString Name;
			GVal->AsObject()->TryGetStringField(TEXT("name"), Name);
			if (Name == TEXT("MyCollapsed"))
			{
				bFoundSubgraphInBasic = true;
			}
		}
	}
	TestFalse(TEXT("Basic list_graphs does NOT include subgraphs"), bFoundSubgraphInBasic);

	// With include_subgraphs: should include the composite subgraph
	TSharedPtr<FJsonObject> SubParams = MakeShared<FJsonObject>();
	SubParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	SubParams->SetBoolField(TEXT("include_subgraphs"), true);

	FCortexCommandResult SubResult = Router.Execute(TEXT("graph.list_graphs"), SubParams);
	TestTrue(TEXT("list_graphs with include_subgraphs succeeds"), SubResult.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* SubGraphs;
	bool bFoundSubgraph = false;
	if (SubResult.Data->TryGetArrayField(TEXT("graphs"), SubGraphs))
	{
		for (const auto& GVal : *SubGraphs)
		{
			const TSharedPtr<FJsonObject>& GObj = GVal->AsObject();
			FString Name;
			GObj->TryGetStringField(TEXT("name"), Name);
			if (Name == TEXT("MyCollapsed"))
			{
				bFoundSubgraph = true;
				FString ParentGraph;
				TestTrue(TEXT("Subgraph entry has parent_graph"),
					GObj->TryGetStringField(TEXT("parent_graph"), ParentGraph));
				TestEqual(TEXT("parent_graph is EventGraph"), ParentGraph, FString(TEXT("EventGraph")));

				FString SubPath;
				TestTrue(TEXT("Subgraph entry has subgraph_path"),
					GObj->TryGetStringField(TEXT("subgraph_path"), SubPath));
				TestEqual(TEXT("subgraph_path is MyCollapsed"), SubPath, FString(TEXT("MyCollapsed")));
			}
		}
	}
	TestTrue(TEXT("list_graphs with include_subgraphs found MyCollapsed"), bFoundSubgraph);

	TestBP->MarkAsGarbage();
	return true;
}

// ── Test 10: Create composite node via AddNode ───────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSubgraphCreateCompositeTest,
	"Cortex.Graph.Subgraph.CreateCompositeViaAddNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSubgraphCreateCompositeTest::RunTest(const FString& Parameters)
{
	using namespace CortexSubgraphTestUtils;

	UEdGraph* EventGraph = nullptr;
	UBlueprint* TestBP = CreateTestBP(TEXT("BP_SubgraphCreateCompositeTest"), EventGraph);
	TestNotNull(TEXT("TestBP created"), TestBP);
	TestNotNull(TEXT("EventGraph found"), EventGraph);

	FCortexCommandRouter Router;
	Router.RegisterDomain(
		TEXT("graph"), TEXT("Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>()
	);

	// Create a composite node via add_node
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	AddParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_Composite"));

	FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
	TestTrue(TEXT("add_node UK2Node_Composite succeeds"), AddResult.bSuccess);

	FString CompositeNodeId;
	if (AddResult.Data)
	{
		AddResult.Data->TryGetStringField(TEXT("node_id"), CompositeNodeId);
	}
	TestFalse(TEXT("Composite node has an ID"), CompositeNodeId.IsEmpty());

	// Find the created composite and verify it has a BoundGraph
	UK2Node_Composite* CompositeNode = nullptr;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		if (Node && Node->GetName() == CompositeNodeId)
		{
			CompositeNode = Cast<UK2Node_Composite>(Node);
			break;
		}
	}
	TestNotNull(TEXT("Composite node found in EventGraph"), CompositeNode);
	if (CompositeNode)
	{
		TestNotNull(TEXT("Composite has a BoundGraph"), CompositeNode->BoundGraph.Get());
	}

	// Verify list_nodes sees the composite with subgraph_name
	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	ListParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));

	FCortexCommandResult ListResult = Router.Execute(TEXT("graph.list_nodes"), ListParams);
	TestTrue(TEXT("list_nodes succeeds"), ListResult.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Nodes;
	bool bFoundCompositeWithSubgraphName = false;
	if (ListResult.Data->TryGetArrayField(TEXT("nodes"), Nodes))
	{
		for (const auto& NodeVal : *Nodes)
		{
			const TSharedPtr<FJsonObject>& NodeObj = NodeVal->AsObject();
			FString ClassName;
			NodeObj->TryGetStringField(TEXT("class"), ClassName);
			if (ClassName == TEXT("K2Node_Composite"))
			{
				FString SubgraphName;
				if (NodeObj->TryGetStringField(TEXT("subgraph_name"), SubgraphName) && !SubgraphName.IsEmpty())
				{
					bFoundCompositeWithSubgraphName = true;

					// Verify we can navigate into the newly created subgraph
					TSharedPtr<FJsonObject> SubParams = MakeShared<FJsonObject>();
					SubParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
					SubParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
					SubParams->SetStringField(TEXT("subgraph_path"), SubgraphName);

					FCortexCommandResult SubResult = Router.Execute(TEXT("graph.list_nodes"), SubParams);
					TestTrue(TEXT("list_nodes inside new composite succeeds"), SubResult.bSuccess);
				}
			}
		}
	}
	TestTrue(TEXT("Composite node has subgraph_name (BoundGraph was created)"), bFoundCompositeWithSubgraphName);

	TestBP->MarkAsGarbage();
	return true;
}
