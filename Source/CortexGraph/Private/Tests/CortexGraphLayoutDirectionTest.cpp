#include "Misc/AutomationTest.h"
#include "CortexGraphLayoutOps.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutDirectionTest,
	"Cortex.Graph.Layout.Direction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutDirectionTest::RunTest(const FString& Parameters)
{
	// Material-style: Source -> Multiply -> Result
	TArray<FCortexLayoutNode> Nodes;

	// "Result" is marked as entry point for layout traversal.
	// With RightToLeft direction, entry points are seeded at layer 0, then layer inversion
	// places them at the rightmost column.
	FCortexLayoutNode Result;
	Result.Id = TEXT("Result");
	Result.bIsEntryPoint = true;
	Result.DataOutputs = {};
	Nodes.Add(Result);

	FCortexLayoutNode Multiply;
	Multiply.Id = TEXT("Multiply");
	Multiply.DataOutputs = {TEXT("Result")};
	Nodes.Add(Multiply);

	FCortexLayoutNode TexSample;
	TexSample.Id = TEXT("TexSample");
	TexSample.DataOutputs = {TEXT("Multiply")};
	Nodes.Add(TexSample);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::RightToLeft;

	FCortexLayoutResult LayoutResult = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestTrue(TEXT("All 3 nodes positioned"), LayoutResult.Positions.Num() == 3);

	// Right-to-left: Result should be rightmost, TexSample leftmost
	TestTrue(TEXT("Result rightmost"),
		LayoutResult.Positions[TEXT("Result")].X > LayoutResult.Positions[TEXT("Multiply")].X);
	TestTrue(TEXT("TexSample leftmost"),
		LayoutResult.Positions[TEXT("TexSample")].X < LayoutResult.Positions[TEXT("Multiply")].X);

	return true;
}
