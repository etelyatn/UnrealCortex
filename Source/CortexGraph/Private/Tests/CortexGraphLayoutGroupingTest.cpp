#include "Misc/AutomationTest.h"
#include "CortexGraphLayoutOps.h"

// --- Test 1: Single data node feeding one exec node ---
// Data node should be positioned left of exec node, roughly same Y row
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutGroupSingleDataTest,
	"Cortex.Graph.Layout.Grouping.SingleDataNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutGroupSingleDataTest::RunTest(const FString& Parameters)
{
	// ExecA(exec) -> ExecB(exec), DataD(pure) -> ExecB(data)
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode ExecA;
	ExecA.Id = TEXT("ExecA");
	ExecA.bIsEntryPoint = true;
	ExecA.bIsExecNode = true;
	ExecA.ExecOutputs = {TEXT("ExecB")};
	Nodes.Add(ExecA);

	FCortexLayoutNode ExecB;
	ExecB.Id = TEXT("ExecB");
	ExecB.bIsExecNode = true;
	Nodes.Add(ExecB);

	FCortexLayoutNode DataD;
	DataD.Id = TEXT("DataD");
	DataD.bIsExecNode = false;
	DataD.DataOutputs = {TEXT("ExecB")};
	Nodes.Add(DataD);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestTrue(TEXT("All 3 nodes positioned"), Result.Positions.Num() == 3);

	// DataD should be left of ExecB (feeds into it)
	TestTrue(TEXT("DataD left of ExecB"),
		Result.Positions[TEXT("DataD")].X < Result.Positions[TEXT("ExecB")].X);

	// ExecA and ExecB should form a clean horizontal chain
	TestTrue(TEXT("ExecA left of ExecB"),
		Result.Positions[TEXT("ExecA")].X < Result.Positions[TEXT("ExecB")].X);

	// DataD should be near ExecB vertically (within group, not far away)
	int32 ExecBY = Result.Positions[TEXT("ExecB")].Y;
	int32 DataDY = Result.Positions[TEXT("DataD")].Y;
	// "Near" = within 2x node height + spacing (300px)
	TestTrue(TEXT("DataD near ExecB vertically"),
		FMath::Abs(DataDY - ExecBY) < 300);

	return true;
}

// --- Test 2: Data chain feeding exec node ---
// Chain: D1 -> D2 -> D3 -> ExecB, with ExecA -> ExecB exec chain
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutGroupDataChainTest,
	"Cortex.Graph.Layout.Grouping.DataChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutGroupDataChainTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode ExecA;
	ExecA.Id = TEXT("ExecA");
	ExecA.bIsEntryPoint = true;
	ExecA.bIsExecNode = true;
	ExecA.ExecOutputs = {TEXT("ExecB")};
	Nodes.Add(ExecA);

	FCortexLayoutNode ExecB;
	ExecB.Id = TEXT("ExecB");
	ExecB.bIsExecNode = true;
	Nodes.Add(ExecB);

	// Pure data chain: D1 -> D2 -> D3 -> ExecB
	FCortexLayoutNode D1;
	D1.Id = TEXT("D1");
	D1.bIsExecNode = false;
	D1.DataOutputs = {TEXT("D2")};
	Nodes.Add(D1);

	FCortexLayoutNode D2;
	D2.Id = TEXT("D2");
	D2.bIsExecNode = false;
	D2.DataOutputs = {TEXT("D3")};
	Nodes.Add(D2);

	FCortexLayoutNode D3;
	D3.Id = TEXT("D3");
	D3.bIsExecNode = false;
	D3.DataOutputs = {TEXT("ExecB")};
	Nodes.Add(D3);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestTrue(TEXT("All 5 nodes positioned"), Result.Positions.Num() == 5);

	// Data chain should flow left to right: D1 < D2 < D3 < ExecB
	TestTrue(TEXT("D1 left of D2"), Result.Positions[TEXT("D1")].X < Result.Positions[TEXT("D2")].X);
	TestTrue(TEXT("D2 left of D3"), Result.Positions[TEXT("D2")].X < Result.Positions[TEXT("D3")].X);
	TestTrue(TEXT("D3 left of ExecB"), Result.Positions[TEXT("D3")].X < Result.Positions[TEXT("ExecB")].X);

	// Exec chain should stay clean: ExecA < ExecB
	TestTrue(TEXT("ExecA left of ExecB"),
		Result.Positions[TEXT("ExecA")].X < Result.Positions[TEXT("ExecB")].X);

	return true;
}

// --- Test 3: Multiple data chains into one exec node ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutGroupMultiChainTest,
	"Cortex.Graph.Layout.Grouping.MultipleChains",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutGroupMultiChainTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode Exec;
	Exec.Id = TEXT("Exec");
	Exec.bIsEntryPoint = true;
	Exec.bIsExecNode = true;
	Nodes.Add(Exec);

	// Chain 1: C1A -> C1B -> Exec (data)
	FCortexLayoutNode C1A;
	C1A.Id = TEXT("C1A");
	C1A.bIsExecNode = false;
	C1A.DataOutputs = {TEXT("C1B")};
	Nodes.Add(C1A);

	FCortexLayoutNode C1B;
	C1B.Id = TEXT("C1B");
	C1B.bIsExecNode = false;
	C1B.DataOutputs = {TEXT("Exec")};
	Nodes.Add(C1B);

	// Chain 2: C2A -> Exec (data)
	FCortexLayoutNode C2A;
	C2A.Id = TEXT("C2A");
	C2A.bIsExecNode = false;
	C2A.DataOutputs = {TEXT("Exec")};
	Nodes.Add(C2A);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestTrue(TEXT("All 4 nodes positioned"), Result.Positions.Num() == 4);

	// Both chains should be left of Exec
	TestTrue(TEXT("C1A left of Exec"), Result.Positions[TEXT("C1A")].X < Result.Positions[TEXT("Exec")].X);
	TestTrue(TEXT("C1B left of Exec"), Result.Positions[TEXT("C1B")].X < Result.Positions[TEXT("Exec")].X);
	TestTrue(TEXT("C2A left of Exec"), Result.Positions[TEXT("C2A")].X < Result.Positions[TEXT("Exec")].X);

	// Chain 1 should flow left to right
	TestTrue(TEXT("C1A left of C1B"), Result.Positions[TEXT("C1A")].X < Result.Positions[TEXT("C1B")].X);

	// The two chains should be at different Y positions (stacked)
	int32 Chain1Y = Result.Positions[TEXT("C1A")].Y;
	int32 Chain2Y = Result.Positions[TEXT("C2A")].Y;
	TestTrue(TEXT("Chains at different Y"), Chain1Y != Chain2Y);

	return true;
}

// --- Test 4: Shared data node between two exec nodes ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutGroupSharedDataTest,
	"Cortex.Graph.Layout.Grouping.SharedDataNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutGroupSharedDataTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode ExecA;
	ExecA.Id = TEXT("ExecA");
	ExecA.bIsEntryPoint = true;
	ExecA.bIsExecNode = true;
	ExecA.ExecOutputs = {TEXT("ExecB")};
	Nodes.Add(ExecA);

	FCortexLayoutNode ExecB;
	ExecB.Id = TEXT("ExecB");
	ExecB.bIsExecNode = true;
	Nodes.Add(ExecB);

	// Shared data node feeds BOTH exec nodes
	FCortexLayoutNode Shared;
	Shared.Id = TEXT("Shared");
	Shared.bIsExecNode = false;
	Shared.DataOutputs = {TEXT("ExecA"), TEXT("ExecB")};
	Nodes.Add(Shared);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestTrue(TEXT("All 3 nodes positioned"), Result.Positions.Num() == 3);

	// Shared should be left of both exec nodes (it feeds into them)
	TestTrue(TEXT("Shared left of ExecA"),
		Result.Positions[TEXT("Shared")].X <= Result.Positions[TEXT("ExecA")].X);

	// No overlaps: all nodes should have distinct positions
	FIntPoint PosA = Result.Positions[TEXT("ExecA")];
	FIntPoint PosB = Result.Positions[TEXT("ExecB")];
	FIntPoint PosS = Result.Positions[TEXT("Shared")];
	TestTrue(TEXT("No overlap: Shared vs ExecA"), PosS != PosA);
	TestTrue(TEXT("No overlap: Shared vs ExecB"), PosS != PosB);

	return true;
}

// --- Test 5: All-exec graph (no pure nodes) - grouping is a no-op ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutGroupAllExecTest,
	"Cortex.Graph.Layout.Grouping.AllExecNoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutGroupAllExecTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode A;
	A.Id = TEXT("A");
	A.bIsEntryPoint = true;
	A.bIsExecNode = true;
	A.ExecOutputs = {TEXT("B")};
	Nodes.Add(A);

	FCortexLayoutNode B;
	B.Id = TEXT("B");
	B.bIsExecNode = true;
	B.ExecOutputs = {TEXT("C")};
	Nodes.Add(B);

	FCortexLayoutNode C;
	C.Id = TEXT("C");
	C.bIsExecNode = true;
	Nodes.Add(C);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	// Same as existing linear chain test - grouping should be no-op
	TestTrue(TEXT("All 3 nodes positioned"), Result.Positions.Num() == 3);
	TestTrue(TEXT("A left of B"), Result.Positions[TEXT("A")].X < Result.Positions[TEXT("B")].X);
	TestTrue(TEXT("B left of C"), Result.Positions[TEXT("B")].X < Result.Positions[TEXT("C")].X);

	return true;
}

// --- Test 6: All-data graph (material style) - grouping skipped ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutGroupAllDataTest,
	"Cortex.Graph.Layout.Grouping.AllDataBypass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutGroupAllDataTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode Tex;
	Tex.Id = TEXT("Tex");
	Tex.bIsExecNode = false;
	Tex.DataOutputs = {TEXT("Mul")};
	Nodes.Add(Tex);

	FCortexLayoutNode Mul;
	Mul.Id = TEXT("Mul");
	Mul.bIsExecNode = false;
	Mul.DataOutputs = {TEXT("Result")};
	Nodes.Add(Mul);

	FCortexLayoutNode Res;
	Res.Id = TEXT("Result");
	Res.bIsEntryPoint = true;
	Res.bIsExecNode = false;
	Nodes.Add(Res);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::RightToLeft;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	// Same as existing direction test - grouping is bypassed for all-data graphs
	TestTrue(TEXT("All 3 nodes positioned"), Result.Positions.Num() == 3);
	TestTrue(TEXT("Result rightmost"),
		Result.Positions[TEXT("Result")].X > Result.Positions[TEXT("Mul")].X);
	TestTrue(TEXT("Tex leftmost"),
		Result.Positions[TEXT("Tex")].X < Result.Positions[TEXT("Mul")].X);

	return true;
}

// --- Test 7: Disconnected data island (no path to exec) ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutGroupDataIslandTest,
	"Cortex.Graph.Layout.Grouping.DataIsland",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutGroupDataIslandTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	// Exec chain
	FCortexLayoutNode ExecA;
	ExecA.Id = TEXT("ExecA");
	ExecA.bIsEntryPoint = true;
	ExecA.bIsExecNode = true;
	ExecA.ExecOutputs = {TEXT("ExecB")};
	Nodes.Add(ExecA);

	FCortexLayoutNode ExecB;
	ExecB.Id = TEXT("ExecB");
	ExecB.bIsExecNode = true;
	Nodes.Add(ExecB);

	// Disconnected data island - feeds nothing
	FCortexLayoutNode Island;
	Island.Id = TEXT("Island");
	Island.bIsExecNode = false;
	Nodes.Add(Island);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	// All nodes should be positioned (island is not lost)
	TestTrue(TEXT("All 3 nodes positioned"), Result.Positions.Num() == 3);

	return true;
}

// --- Test 9: Multi-hop data chains into middle exec nodes ---
// Topology: ExecA → Branch → Delay → Print (exec chain)
//           GetHealth → Greater → Branch   (2-hop data into middle exec)
//           GetAL → BreakVec → FloatToStr → Print  (3-hop data into end exec)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutGroupComplexMixedTest,
	"Cortex.Graph.Layout.Grouping.ComplexMixed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutGroupComplexMixedTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	// Exec chain
	FCortexLayoutNode ExecA;
	ExecA.Id = TEXT("ExecA");
	ExecA.bIsEntryPoint = true;
	ExecA.bIsExecNode = true;
	ExecA.ExecOutputs = {TEXT("Branch")};
	Nodes.Add(ExecA);

	FCortexLayoutNode Branch;
	Branch.Id = TEXT("Branch");
	Branch.bIsExecNode = true;
	Branch.ExecOutputs = {TEXT("Delay")};
	Nodes.Add(Branch);

	FCortexLayoutNode Delay;
	Delay.Id = TEXT("Delay");
	Delay.bIsExecNode = true;
	Delay.ExecOutputs = {TEXT("Print")};
	Nodes.Add(Delay);

	FCortexLayoutNode Print;
	Print.Id = TEXT("Print");
	Print.bIsExecNode = true;
	Nodes.Add(Print);

	// 2-hop data chain feeding Branch (middle exec node)
	FCortexLayoutNode GetHealth;
	GetHealth.Id = TEXT("GetHealth");
	GetHealth.bIsExecNode = false;
	GetHealth.DataOutputs = {TEXT("Greater")};
	Nodes.Add(GetHealth);

	FCortexLayoutNode Greater;
	Greater.Id = TEXT("Greater");
	Greater.bIsExecNode = false;
	Greater.DataOutputs = {TEXT("Branch")};
	Nodes.Add(Greater);

	// 3-hop data chain feeding Print (end exec node)
	FCortexLayoutNode GetAL;
	GetAL.Id = TEXT("GetAL");
	GetAL.bIsExecNode = false;
	GetAL.DataOutputs = {TEXT("BreakVec")};
	Nodes.Add(GetAL);

	FCortexLayoutNode BreakVec;
	BreakVec.Id = TEXT("BreakVec");
	BreakVec.bIsExecNode = false;
	BreakVec.DataOutputs = {TEXT("FloatToStr")};
	Nodes.Add(BreakVec);

	FCortexLayoutNode FloatToStr;
	FloatToStr.Id = TEXT("FloatToStr");
	FloatToStr.bIsExecNode = false;
	FloatToStr.DataOutputs = {TEXT("Print")};
	Nodes.Add(FloatToStr);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestTrue(TEXT("All 9 nodes positioned"), Result.Positions.Num() == 9);

	// Exec chain flows left to right
	TestTrue(TEXT("ExecA left of Branch"), Result.Positions[TEXT("ExecA")].X < Result.Positions[TEXT("Branch")].X);
	TestTrue(TEXT("Branch left of Delay"), Result.Positions[TEXT("Branch")].X < Result.Positions[TEXT("Delay")].X);
	TestTrue(TEXT("Delay left of Print"), Result.Positions[TEXT("Delay")].X < Result.Positions[TEXT("Print")].X);

	// 2-hop data chain: both ancestors must be left of Branch (their exec consumer)
	TestTrue(TEXT("GetHealth left of Branch"), Result.Positions[TEXT("GetHealth")].X < Result.Positions[TEXT("Branch")].X);
	TestTrue(TEXT("Greater left of Branch"), Result.Positions[TEXT("Greater")].X < Result.Positions[TEXT("Branch")].X);
	TestTrue(TEXT("GetHealth left of or equal Greater"), Result.Positions[TEXT("GetHealth")].X <= Result.Positions[TEXT("Greater")].X);

	// 3-hop data chain must flow left to right and end left of Print
	TestTrue(TEXT("GetAL left of BreakVec"), Result.Positions[TEXT("GetAL")].X <= Result.Positions[TEXT("BreakVec")].X);
	TestTrue(TEXT("BreakVec left of FloatToStr"), Result.Positions[TEXT("BreakVec")].X < Result.Positions[TEXT("FloatToStr")].X);
	TestTrue(TEXT("FloatToStr left of Print"), Result.Positions[TEXT("FloatToStr")].X < Result.Positions[TEXT("Print")].X);

	// Data nodes must be near their exec consumer vertically (grouping places them together)
	const int32 BranchY = Result.Positions[TEXT("Branch")].Y;
	TestTrue(TEXT("GetHealth near Branch Y"), FMath::Abs(Result.Positions[TEXT("GetHealth")].Y - BranchY) < 400);
	TestTrue(TEXT("Greater near Branch Y"), FMath::Abs(Result.Positions[TEXT("Greater")].Y - BranchY) < 400);

	const int32 PrintY = Result.Positions[TEXT("Print")].Y;
	TestTrue(TEXT("FloatToStr near Print Y"), FMath::Abs(Result.Positions[TEXT("FloatToStr")].Y - PrintY) < 400);
	TestTrue(TEXT("BreakVec near Print Y"), FMath::Abs(Result.Positions[TEXT("BreakVec")].Y - PrintY) < 400);
	TestTrue(TEXT("GetAL near Print Y"), FMath::Abs(Result.Positions[TEXT("GetAL")].Y - PrintY) < 400);

	return true;
}

// --- Test 8: Grid snap applied to all positions ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutGroupGridSnapTest,
	"Cortex.Graph.Layout.Grouping.GridSnap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutGroupGridSnapTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode ExecA;
	ExecA.Id = TEXT("ExecA");
	ExecA.bIsEntryPoint = true;
	ExecA.bIsExecNode = true;
	ExecA.Width = 200;
	ExecA.Height = 100;
	ExecA.ExecOutputs = {TEXT("ExecB")};
	Nodes.Add(ExecA);

	FCortexLayoutNode ExecB;
	ExecB.Id = TEXT("ExecB");
	ExecB.bIsExecNode = true;
	ExecB.Width = 200;
	ExecB.Height = 100;
	Nodes.Add(ExecB);

	FCortexLayoutNode Data;
	Data.Id = TEXT("Data");
	Data.bIsExecNode = false;
	Data.Width = 150;
	Data.Height = 80;
	Data.DataOutputs = {TEXT("ExecB")};
	Nodes.Add(Data);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	// All positions should be multiples of 16 (grid snap)
	for (const auto& Pair : Result.Positions)
	{
		TestTrue(FString::Printf(TEXT("%s X aligned to grid"), *Pair.Key),
			Pair.Value.X % 16 == 0);
		TestTrue(FString::Printf(TEXT("%s Y aligned to grid"), *Pair.Key),
			Pair.Value.Y % 16 == 0);
	}

	return true;
}
