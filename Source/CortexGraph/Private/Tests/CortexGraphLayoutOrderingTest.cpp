#include "Misc/AutomationTest.h"
#include "CortexGraphLayoutOps.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutOrderingTest,
	"Cortex.Graph.Layout.Ordering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutOrderingTest::RunTest(const FString& Parameters)
{
	// Diamond pattern: A -> B, A -> C, B -> D, C -> D
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode NodeA;
	NodeA.Id = TEXT("A");
	NodeA.Width = 150; NodeA.Height = 100;
	NodeA.bIsEntryPoint = true;
	NodeA.ExecOutputs = {TEXT("B"), TEXT("C")};
	Nodes.Add(NodeA);

	FCortexLayoutNode NodeB;
	NodeB.Id = TEXT("B");
	NodeB.Width = 150; NodeB.Height = 100;
	NodeB.ExecOutputs = {TEXT("D")};
	Nodes.Add(NodeB);

	FCortexLayoutNode NodeC;
	NodeC.Id = TEXT("C");
	NodeC.Width = 150; NodeC.Height = 100;
	NodeC.ExecOutputs = {TEXT("D")};
	Nodes.Add(NodeC);

	FCortexLayoutNode NodeD;
	NodeD.Id = TEXT("D");
	NodeD.Width = 150; NodeD.Height = 100;
	Nodes.Add(NodeD);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;
	Config.HorizontalSpacing = 80;
	Config.VerticalSpacing = 40;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	// All 4 nodes placed
	TestTrue(TEXT("All 4 nodes should have positions"), Result.Positions.Num() == 4);

	// B and C should be in the same column (both layer 1)
	TestTrue(TEXT("B and C same X"), Result.Positions[TEXT("B")].X == Result.Positions[TEXT("C")].X);

	// B and C should not overlap vertically
	int32 BY = Result.Positions[TEXT("B")].Y;
	int32 CY = Result.Positions[TEXT("C")].Y;
	TestTrue(TEXT("B and C should not overlap"), FMath::Abs(BY - CY) >= 100 + 40); // Height + spacing

	// D should be in the rightmost column
	TestTrue(TEXT("D right of B"), Result.Positions[TEXT("D")].X > Result.Positions[TEXT("B")].X);

	// A should be centered vertically relative to B and C
	int32 AY = Result.Positions[TEXT("A")].Y;
	int32 MidBC = (BY + CY) / 2;
	TestTrue(TEXT("A roughly centered with B/C"), FMath::Abs(AY - MidBC) <= 50);

	return true;
}
