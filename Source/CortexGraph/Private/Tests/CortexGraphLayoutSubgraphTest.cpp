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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutIncrementalCollisionTest,
	"Cortex.Graph.Layout.IncrementalCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutIncrementalCollisionTest::RunTest(const FString& Parameters)
{
	FCortexLayoutNode A;
	A.Id = TEXT("A");
	A.bIsExecNode = true;
	A.Width = 250;
	A.Height = 206;
	A.ExecOutputs.Add(TEXT("B"));
	FCortexLayoutNode B;
	B.Id = TEXT("B");
	B.bIsExecNode = true;
	B.Width = 250;
	B.Height = 206;
	const TArray<FCortexLayoutNode> Nodes = { A, B };
	FCortexLayoutConfig Config;
	const FCortexLayoutResult Full = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);
	TMap<FString, FIntPoint> Existing;
	Existing.Add(A.Id, Full.Positions[B.Id]);
	Existing.Add(B.Id, FIntPoint::ZeroValue);
	Config.Mode = ECortexLayoutMode::Incremental;
	const FCortexLayoutResult Actual = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config, Existing);
	TestFalse(TEXT("Established A is not moved"), Actual.Positions.Contains(A.Id));
	const FIntPoint* New = Actual.Positions.Find(B.Id);
	if (!TestNotNull(TEXT("New B is positioned"), New))
	{
		return false;
	}
	const FIntPoint Fixed = Existing[A.Id];
	TestTrue(TEXT("New body has requested separation"),
		New->X >= Fixed.X + A.Width + Config.HorizontalSpacing ||
		Fixed.X >= New->X + B.Width + Config.HorizontalSpacing ||
		New->Y >= Fixed.Y + A.Height + Config.VerticalSpacing ||
		Fixed.Y >= New->Y + B.Height + Config.VerticalSpacing);
	TestNotEqual(TEXT("Placed node does not retain unpositioned sentinel"), *New, FIntPoint::ZeroValue);
	Existing.Add(B.Id, *New);
	const FCortexLayoutResult Repeated = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config, Existing);
	TestEqual(TEXT("Repeated incremental layout moves no established nodes"), Repeated.Positions.Num(), 0);

	FCortexLayoutNode NegativeA;
	NegativeA.Id = TEXT("A");
	NegativeA.bIsExecNode = true;
	NegativeA.Width = 150;
	NegativeA.Height = 100;
	FCortexLayoutNode NegativeB = NegativeA;
	NegativeB.Id = TEXT("B");
	NegativeB.ExecOutputs.Add(NegativeA.Id);
	const TArray<FCortexLayoutNode> NegativeNodes = { NegativeA, NegativeB };
	TMap<FString, FIntPoint> NegativeExisting;
	NegativeExisting.Add(NegativeA.Id, FIntPoint(0, -140));
	NegativeExisting.Add(NegativeB.Id, FIntPoint::ZeroValue);
	const FCortexLayoutResult NegativeResult = FCortexGraphLayoutOps::CalculateLayout(
		NegativeNodes, Config, NegativeExisting);
	const FIntPoint* NegativePosition = NegativeResult.Positions.Find(NegativeB.Id);
	if (TestNotNull(TEXT("Negative obstacle placement returned"), NegativePosition))
	{
		TestNotEqual(TEXT("Collision displacement cannot restore the sentinel"),
			*NegativePosition, FIntPoint::ZeroValue);
		TestTrue(TEXT("Negative obstacle retains body clearance"),
			NegativePosition->Y >= -140 + NegativeA.Height + Config.VerticalSpacing);
		NegativeExisting.Add(NegativeB.Id, *NegativePosition);
		const FCortexLayoutResult NegativeRepeat = FCortexGraphLayoutOps::CalculateLayout(
			NegativeNodes, Config, NegativeExisting);
		TestEqual(TEXT("Negative obstacle placement is established on repeat"),
			NegativeRepeat.Positions.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphLayoutGroupedBodiesTest,
	"Cortex.Graph.Layout.GroupedBodySeparation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphLayoutGroupedBodiesTest::RunTest(const FString& Parameters)
{
	TArray<FCortexLayoutNode> Nodes;
	FCortexLayoutNode Entry;
	Entry.Id = TEXT("Entry");
	Entry.bIsEntryPoint = true;
	Entry.bIsExecNode = true;
	Entry.ExecOutputs = { TEXT("Branch") };
	Nodes.Add(Entry);
	FCortexLayoutNode Branch;
	Branch.Id = TEXT("Branch");
	Branch.bIsExecNode = true;
	Branch.ExecOutputs = { TEXT("Left"), TEXT("Right") };
	Nodes.Add(Branch);
	for (const FString& Id : { FString(TEXT("Left")), FString(TEXT("Right")) })
	{
		FCortexLayoutNode Exec;
		Exec.Id = Id;
		Exec.Width = 250;
		Exec.Height = 180;
		Exec.bIsExecNode = true;
		Nodes.Add(Exec);
		for (int32 Index = 0; Index < 3; ++Index)
		{
			FCortexLayoutNode Pure;
			Pure.Id = FString::Printf(TEXT("%sPure%d"), *Id, Index);
			Pure.Width = 180 + Index * 40;
			Pure.Height = 120 + Index * 60;
			Pure.DataOutputs.Add(Id);
			Nodes.Add(Pure);
		}
	}
	FCortexLayoutNode Disconnected;
	Disconnected.Id = TEXT("Disconnected");
	Disconnected.Width = 350;
	Disconnected.Height = 400;
	Nodes.Add(Disconnected);
	FCortexLayoutConfig Config;
	Config.HorizontalSpacing = 91;
	Config.VerticalSpacing = 47;
	const FCortexLayoutResult Result = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);
	const FCortexLayoutResult Repeat = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config);
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FCortexLayoutNode& A = Nodes[Index];
		const FIntPoint* AP = Result.Positions.Find(A.Id);
		if (!TestNotNull(*FString::Printf(TEXT("%s is positioned"), *A.Id), AP))
		{
			return false;
		}
		TestEqual(*FString::Printf(TEXT("%s repeated position is stable"), *A.Id), Repeat.Positions[A.Id], *AP);
		for (int32 Other = Index + 1; Other < Nodes.Num(); ++Other)
		{
			const FCortexLayoutNode& B = Nodes[Other];
			const FIntPoint* BP = Result.Positions.Find(B.Id);
			if (!TestNotNull(*FString::Printf(TEXT("%s is positioned"), *B.Id), BP))
			{
				return false;
			}
			TestTrue(*FString::Printf(TEXT("%s / %s bodies have clearance"), *A.Id, *B.Id),
				AP->X >= BP->X + B.Width + Config.HorizontalSpacing ||
				BP->X >= AP->X + A.Width + Config.HorizontalSpacing ||
				AP->Y >= BP->Y + B.Height + Config.VerticalSpacing ||
				BP->Y >= AP->Y + A.Height + Config.VerticalSpacing);
		}
	}
	return true;
}
