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
