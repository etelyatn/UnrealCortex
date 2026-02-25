#include "Misc/AutomationTest.h"
#include "CortexGraphLayoutOps.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutLayerAssignmentTest,
	"Cortex.Graph.Layout.Refinement.LayerAssignmentPopulated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutLayerAssignmentTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode A;
	A.Id = TEXT("A");
	A.Width = 150;
	A.Height = 100;
	A.bIsEntryPoint = true;
	A.bIsExecNode = true;
	A.ExecOutputs = { TEXT("B") };
	Nodes.Add(A);

	FCortexLayoutNode B;
	B.Id = TEXT("B");
	B.Width = 150;
	B.Height = 100;
	B.bIsExecNode = true;
	B.ExecOutputs = { TEXT("C") };
	Nodes.Add(B);

	FCortexLayoutNode C;
	C.Id = TEXT("C");
	C.Width = 150;
	C.Height = 100;
	C.bIsExecNode = true;
	Nodes.Add(C);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestEqual(TEXT("LayerAssignment should have 3 entries"), Result.LayerAssignment.Num(), 3);
	TestTrue(TEXT("A should have layer assignment"), Result.LayerAssignment.Contains(TEXT("A")));
	TestTrue(TEXT("B should have layer assignment"), Result.LayerAssignment.Contains(TEXT("B")));
	TestTrue(TEXT("C should have layer assignment"), Result.LayerAssignment.Contains(TEXT("C")));
	TestTrue(TEXT("A layer < B layer"), Result.LayerAssignment[TEXT("A")] < Result.LayerAssignment[TEXT("B")]);
	TestTrue(TEXT("B layer < C layer"), Result.LayerAssignment[TEXT("B")] < Result.LayerAssignment[TEXT("C")]);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutLinearChainStraightnessTest,
	"Cortex.Graph.Layout.Refinement.LinearChainStraightness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutLinearChainStraightnessTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	auto MakeNode = [&](const FString& Id, int32 W = 150, int32 H = 100)
	{
		FCortexLayoutNode N;
		N.Id = Id;
		N.Width = W;
		N.Height = H;
		N.bIsExecNode = false;
		return N;
	};

	FCortexLayoutNode A = MakeNode(TEXT("A"));
	A.DataOutputs = { TEXT("B") };
	Nodes.Add(A);

	FCortexLayoutNode S1 = MakeNode(TEXT("S1"));
	S1.DataOutputs = { TEXT("B") };
	Nodes.Add(S1);

	FCortexLayoutNode B = MakeNode(TEXT("B"));
	B.DataOutputs = { TEXT("C") };
	Nodes.Add(B);

	FCortexLayoutNode S2 = MakeNode(TEXT("S2"));
	S2.DataOutputs = { TEXT("C") };
	Nodes.Add(S2);

	FCortexLayoutNode C = MakeNode(TEXT("C"));
	C.DataOutputs = { TEXT("D") };
	Nodes.Add(C);

	FCortexLayoutNode D = MakeNode(TEXT("D"));
	D.bIsEntryPoint = true;
	Nodes.Add(D);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::RightToLeft;
	Config.VerticalSpacing = 60;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestEqual(TEXT("All 6 nodes positioned"), Result.Positions.Num(), 6);

	const int32 AY = Result.Positions[TEXT("A")].Y;
	const int32 BY = Result.Positions[TEXT("B")].Y;
	const int32 CY = Result.Positions[TEXT("C")].Y;
	const int32 DY = Result.Positions[TEXT("D")].Y;
	const int32 Tolerance = CortexGraphLayout::GridSnapSize * 2;

	TestTrue(TEXT("A and B at similar Y (main chain straight)"), FMath::Abs(AY - BY) <= Tolerance);
	TestTrue(TEXT("B and C at similar Y (main chain straight)"), FMath::Abs(BY - CY) <= Tolerance);
	TestTrue(TEXT("C and D at similar Y (main chain straight)"), FMath::Abs(CY - DY) <= Tolerance);
	TestTrue(TEXT("S1 not at same Y as B"), FMath::Abs(Result.Positions[TEXT("S1")].Y - BY) > 0);
	TestTrue(TEXT("S2 not at same Y as C"), FMath::Abs(Result.Positions[TEXT("S2")].Y - CY) > 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutGroupDeltaCascadeTest,
	"Cortex.Graph.Layout.Refinement.GroupDeltaCascade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutGroupDeltaCascadeTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode ExecA;
	ExecA.Id = TEXT("ExecA");
	ExecA.bIsEntryPoint = true;
	ExecA.bIsExecNode = true;
	ExecA.ExecOutputs = { TEXT("ExecB") };
	ExecA.Width = 200;
	ExecA.Height = 100;
	Nodes.Add(ExecA);

	FCortexLayoutNode ExecB;
	ExecB.Id = TEXT("ExecB");
	ExecB.bIsExecNode = true;
	ExecB.Width = 200;
	ExecB.Height = 100;
	Nodes.Add(ExecB);

	FCortexLayoutNode DataX;
	DataX.Id = TEXT("DataX");
	DataX.bIsExecNode = false;
	DataX.DataOutputs = { TEXT("ExecB") };
	DataX.Width = 150;
	DataX.Height = 80;
	Nodes.Add(DataX);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestEqual(TEXT("All 3 nodes positioned"), Result.Positions.Num(), 3);

	const int32 ExecBY = Result.Positions[TEXT("ExecB")].Y;
	const int32 DataXY = Result.Positions[TEXT("DataX")].Y;
	const int32 ExecBCenter = ExecBY + 50;
	const int32 DataXCenter = DataXY + 40;

	TestTrue(TEXT("DataX near ExecB after refinement"), FMath::Abs(DataXCenter - ExecBCenter) < 200);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutBranchAlignmentTest,
	"Cortex.Graph.Layout.Refinement.BranchNodeAlignment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutBranchAlignmentTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode A;
	A.Id = TEXT("A");
	A.Width = 150;
	A.Height = 100;
	A.bIsExecNode = false;
	A.DataOutputs = { TEXT("C") };
	Nodes.Add(A);

	FCortexLayoutNode B;
	B.Id = TEXT("B");
	B.Width = 150;
	B.Height = 100;
	B.bIsExecNode = false;
	B.DataOutputs = { TEXT("C") };
	Nodes.Add(B);

	FCortexLayoutNode C;
	C.Id = TEXT("C");
	C.Width = 150;
	C.Height = 100;
	C.bIsExecNode = false;
	C.DataOutputs = { TEXT("D") };
	Nodes.Add(C);

	FCortexLayoutNode D;
	D.Id = TEXT("D");
	D.Width = 150;
	D.Height = 100;
	D.bIsExecNode = false;
	D.bIsEntryPoint = true;
	Nodes.Add(D);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::RightToLeft;
	Config.VerticalSpacing = 60;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestEqual(TEXT("All 4 nodes positioned"), Result.Positions.Num(), 4);

	const int32 ACenterY = Result.Positions[TEXT("A")].Y + 50;
	const int32 BCenterY = Result.Positions[TEXT("B")].Y + 50;
	const int32 CCenterY = Result.Positions[TEXT("C")].Y + 50;
	const int32 MedianCenter = (ACenterY + BCenterY) / 2;
	const int32 MedianTolerance = CortexGraphLayout::GridSnapSize * 4;
	TestTrue(TEXT("C centered between A and B (within 64px)"),
		FMath::Abs(CCenterY - MedianCenter) <= MedianTolerance);

	const int32 DCenterY = Result.Positions[TEXT("D")].Y + 50;
	TestTrue(TEXT("D aligned with C (within 32px)"),
		FMath::Abs(DCenterY - CCenterY) <= CortexGraphLayout::GridSnapSize * 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutNoOverlapTest,
	"Cortex.Graph.Layout.Refinement.NoOverlapAfterRefinement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutNoOverlapTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode A;
	A.Id = TEXT("A");
	A.Width = 150;
	A.Height = 100;
	A.bIsExecNode = false;
	A.DataOutputs = { TEXT("B"), TEXT("C"), TEXT("D") };
	Nodes.Add(A);

	FCortexLayoutNode B;
	B.Id = TEXT("B");
	B.Width = 150;
	B.Height = 120;
	B.bIsExecNode = false;
	B.bIsEntryPoint = true;
	Nodes.Add(B);

	FCortexLayoutNode C;
	C.Id = TEXT("C");
	C.Width = 150;
	C.Height = 80;
	C.bIsExecNode = false;
	C.bIsEntryPoint = true;
	Nodes.Add(C);

	FCortexLayoutNode D;
	D.Id = TEXT("D");
	D.Width = 150;
	D.Height = 150;
	D.bIsExecNode = false;
	D.bIsEntryPoint = true;
	Nodes.Add(D);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::RightToLeft;
	Config.VerticalSpacing = 40;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestEqual(TEXT("All 4 nodes positioned"), Result.Positions.Num(), 4);

	TArray<FString> SameLayer = { TEXT("B"), TEXT("C"), TEXT("D") };
	TMap<FString, int32> Heights;
	Heights.Add(TEXT("B"), 120);
	Heights.Add(TEXT("C"), 80);
	Heights.Add(TEXT("D"), 150);

	for (int32 i = 0; i < SameLayer.Num(); ++i)
	{
		for (int32 j = i + 1; j < SameLayer.Num(); ++j)
		{
			const int32 YI = Result.Positions[SameLayer[i]].Y;
			const int32 HI = Heights[SameLayer[i]];
			const int32 YJ = Result.Positions[SameLayer[j]].Y;
			const int32 HJ = Heights[SameLayer[j]];
			const bool bOverlap = (YI < YJ + HJ) && (YJ < YI + HI);
			TestFalse(FString::Printf(TEXT("%s and %s should not overlap"), *SameLayer[i], *SameLayer[j]), bOverlap);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutSubgraphIsolationTest,
	"Cortex.Graph.Layout.Refinement.SubgraphIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutSubgraphIsolationTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode A;
	A.Id = TEXT("A");
	A.Width = 150;
	A.Height = 100;
	A.bIsExecNode = false;
	A.DataOutputs = { TEXT("B") };
	Nodes.Add(A);

	FCortexLayoutNode B;
	B.Id = TEXT("B");
	B.Width = 150;
	B.Height = 100;
	B.bIsExecNode = false;
	B.bIsEntryPoint = true;
	Nodes.Add(B);

	FCortexLayoutNode C;
	C.Id = TEXT("C");
	C.Width = 150;
	C.Height = 100;
	C.bIsExecNode = false;
	C.DataOutputs = { TEXT("D") };
	Nodes.Add(C);

	FCortexLayoutNode D;
	D.Id = TEXT("D");
	D.Width = 150;
	D.Height = 100;
	D.bIsExecNode = false;
	D.bIsEntryPoint = true;
	Nodes.Add(D);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::RightToLeft;
	Config.VerticalSpacing = 40;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestEqual(TEXT("All 4 nodes positioned"), Result.Positions.Num(), 4);

	const int32 S1MinY = FMath::Min(Result.Positions[TEXT("A")].Y, Result.Positions[TEXT("B")].Y);
	const int32 S1MaxY = FMath::Max(Result.Positions[TEXT("A")].Y + 100, Result.Positions[TEXT("B")].Y + 100);
	const int32 S2MinY = FMath::Min(Result.Positions[TEXT("C")].Y, Result.Positions[TEXT("D")].Y);
	const int32 S2MaxY = FMath::Max(Result.Positions[TEXT("C")].Y + 100, Result.Positions[TEXT("D")].Y + 100);
	const bool bNoOverlap = (S1MaxY <= S2MinY) || (S2MaxY <= S1MinY);
	TestTrue(TEXT("Subgraphs should not overlap vertically"), bNoOverlap);

	TestTrue(TEXT("A and B at same Y (linear chain)"),
		FMath::Abs(Result.Positions[TEXT("A")].Y - Result.Positions[TEXT("B")].Y)
		<= CortexGraphLayout::GridSnapSize);
	TestTrue(TEXT("C and D at same Y (linear chain)"),
		FMath::Abs(Result.Positions[TEXT("C")].Y - Result.Positions[TEXT("D")].Y)
		<= CortexGraphLayout::GridSnapSize);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutNoNewCrossingsTest,
	"Cortex.Graph.Layout.Refinement.NoNewCrossings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutNoNewCrossingsTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;

	FCortexLayoutNode A;
	A.Id = TEXT("A");
	A.Width = 150;
	A.Height = 100;
	A.bIsExecNode = false;
	A.DataOutputs = { TEXT("B"), TEXT("C") };
	Nodes.Add(A);

	FCortexLayoutNode B;
	B.Id = TEXT("B");
	B.Width = 150;
	B.Height = 100;
	B.bIsExecNode = false;
	B.DataOutputs = { TEXT("D") };
	Nodes.Add(B);

	FCortexLayoutNode C;
	C.Id = TEXT("C");
	C.Width = 150;
	C.Height = 100;
	C.bIsExecNode = false;
	C.DataOutputs = { TEXT("D") };
	Nodes.Add(C);

	FCortexLayoutNode D;
	D.Id = TEXT("D");
	D.Width = 150;
	D.Height = 100;
	D.bIsExecNode = false;
	D.bIsEntryPoint = true;
	Nodes.Add(D);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::RightToLeft;
	Config.VerticalSpacing = 40;

	FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);

	TestEqual(TEXT("All 4 nodes positioned"), Result.Positions.Num(), 4);

	const int32 BY = Result.Positions[TEXT("B")].Y;
	const int32 CY = Result.Positions[TEXT("C")].Y;
	TestTrue(TEXT("B and C at different Y positions"), BY != CY);

	const int32 Gap = FMath::Abs(BY - CY);
	TestTrue(TEXT("B and C have sufficient gap (no overlap)"), Gap >= 100);

	return true;
}
