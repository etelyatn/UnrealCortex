#include "Misc/AutomationTest.h"
#include "CortexGraphLayoutOps.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutLayerTest,
	"Cortex.Graph.Layout.LayerAssignment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutLayerTest::RunTest(const FString& Parameters)
{
	// Simple linear chain: A -> B -> C (exec flow)
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode NodeA;
	NodeA.Id = TEXT("A");
	NodeA.bIsEntryPoint = true;
	NodeA.ExecOutputs = {TEXT("B")};
	Nodes.Add(NodeA);

	FCortexLayoutNode NodeB;
	NodeB.Id = TEXT("B");
	NodeB.ExecOutputs = {TEXT("C")};
	Nodes.Add(NodeB);

	FCortexLayoutNode NodeC;
	NodeC.Id = TEXT("C");
	Nodes.Add(NodeC);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	// A should be leftmost, C rightmost
	TestTrue(TEXT("All 3 nodes should have positions"), Result.Positions.Num() == 3);
	TestTrue(TEXT("A should be left of B"), Result.Positions[TEXT("A")].X < Result.Positions[TEXT("B")].X);
	TestTrue(TEXT("B should be left of C"), Result.Positions[TEXT("B")].X < Result.Positions[TEXT("C")].X);

	// Test with data-only node: D feeds into B (data, no exec)
	FCortexLayoutNode NodeD;
	NodeD.Id = TEXT("D");
	NodeD.DataOutputs = {TEXT("B")};
	Nodes.Add(NodeD);

	Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	// D should be in the column before B (or same column as A)
	TestTrue(TEXT("All 4 nodes should have positions"), Result.Positions.Num() == 4);
	TestTrue(TEXT("D should be left of or equal to B"), Result.Positions[TEXT("D")].X <= Result.Positions[TEXT("B")].X);

	// Test Sequence fan-out: A -> B -> D -> F, A -> C -> E -> D
	// D must be at layer 3 (longest path), F at layer 4
	TArray<FCortexLayoutNode> FanOutNodes;

	FCortexLayoutNode FA; FA.Id = TEXT("A"); FA.bIsEntryPoint = true;
	FA.ExecOutputs = {TEXT("B"), TEXT("C")}; FanOutNodes.Add(FA);

	FCortexLayoutNode FB; FB.Id = TEXT("B"); FB.ExecOutputs = {TEXT("D")}; FanOutNodes.Add(FB);
	FCortexLayoutNode FC; FC.Id = TEXT("C"); FC.ExecOutputs = {TEXT("E")}; FanOutNodes.Add(FC);
	FCortexLayoutNode FE; FE.Id = TEXT("E"); FE.ExecOutputs = {TEXT("D")}; FanOutNodes.Add(FE);
	FCortexLayoutNode FD; FD.Id = TEXT("D"); FD.ExecOutputs = {TEXT("F")}; FanOutNodes.Add(FD);
	FCortexLayoutNode FF; FF.Id = TEXT("F"); FanOutNodes.Add(FF);

	FCortexLayoutResult FanResult = FCortexGraphLayoutOps::CalculateLayout(FanOutNodes, Config);

	TestTrue(TEXT("Fan-out: D right of E"), FanResult.Positions[TEXT("D")].X > FanResult.Positions[TEXT("E")].X);
	TestTrue(TEXT("Fan-out: F right of D"), FanResult.Positions[TEXT("F")].X > FanResult.Positions[TEXT("D")].X);
	TestTrue(TEXT("Fan-out: D right of B"), FanResult.Positions[TEXT("D")].X > FanResult.Positions[TEXT("B")].X);

	return true;
}
