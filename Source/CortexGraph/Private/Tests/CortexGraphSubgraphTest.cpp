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
