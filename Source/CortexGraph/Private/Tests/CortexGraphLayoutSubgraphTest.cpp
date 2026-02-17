#include "Misc/AutomationTest.h"
#include "CortexGraphLayoutOps.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutSubgraphTest,
	"Cortex.Graph.Layout.Subgraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutSubgraphTest::RunTest(const FString& Parameters)
{
	// Two disconnected chains: A -> B and C -> D
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode NodeA;
	NodeA.Id = TEXT("A");
	NodeA.bIsEntryPoint = true;
	NodeA.ExecOutputs = {TEXT("B")};
	Nodes.Add(NodeA);

	FCortexLayoutNode NodeB;
	NodeB.Id = TEXT("B");
	Nodes.Add(NodeB);

	FCortexLayoutNode NodeC;
	NodeC.Id = TEXT("C");
	NodeC.bIsEntryPoint = true;
	NodeC.ExecOutputs = {TEXT("D")};
	Nodes.Add(NodeC);

	FCortexLayoutNode NodeD;
	NodeD.Id = TEXT("D");
	Nodes.Add(NodeD);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	// All 4 nodes positioned
	TestTrue(TEXT("All 4 nodes should have positions"), Result.Positions.Num() == 4);

	// Subgraphs should not overlap vertically
	int32 AB_MinY = FMath::Min(Result.Positions[TEXT("A")].Y, Result.Positions[TEXT("B")].Y);
	int32 AB_MaxY = FMath::Max(Result.Positions[TEXT("A")].Y + 100, Result.Positions[TEXT("B")].Y + 100);
	int32 CD_MinY = FMath::Min(Result.Positions[TEXT("C")].Y, Result.Positions[TEXT("D")].Y);
	int32 CD_MaxY = FMath::Max(Result.Positions[TEXT("C")].Y + 100, Result.Positions[TEXT("D")].Y + 100);

	bool bNoOverlap = (AB_MaxY <= CD_MinY) || (CD_MaxY <= AB_MinY);
	TestTrue(TEXT("Subgraphs should not overlap vertically"), bNoOverlap);

	// --- Test Incremental Mode ---
	TMap<FString, FIntPoint> ExistingPositions;
	ExistingPositions.Add(TEXT("A"), FIntPoint(100, 200));
	ExistingPositions.Add(TEXT("B"), FIntPoint(400, 200));
	ExistingPositions.Add(TEXT("C"), FIntPoint(0, 0));  // "New" node
	ExistingPositions.Add(TEXT("D"), FIntPoint(0, 0));  // "New" node

	FCortexLayoutConfig IncrConfig;
	IncrConfig.Direction = ECortexLayoutDirection::LeftToRight;
	IncrConfig.Mode = ECortexLayoutMode::Incremental;

	FCortexLayoutResult IncrResult = FCortexGraphLayoutOps::CalculateLayout(Nodes, IncrConfig, ExistingPositions);

	// Only C and D should be in the result (A and B already positioned)
	TestFalse(TEXT("A should NOT be repositioned"), IncrResult.Positions.Contains(TEXT("A")));
	TestFalse(TEXT("B should NOT be repositioned"), IncrResult.Positions.Contains(TEXT("B")));
	TestTrue(TEXT("C should be positioned"), IncrResult.Positions.Contains(TEXT("C")));
	TestTrue(TEXT("D should be positioned"), IncrResult.Positions.Contains(TEXT("D")));

	return true;
}
