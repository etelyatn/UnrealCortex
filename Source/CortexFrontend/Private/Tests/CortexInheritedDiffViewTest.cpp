#include "Misc/AutomationTest.h"
#include "Widgets/SCortexInheritedDiffView.h"

// Test the diff computation logic (static helper, no widget required)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexLineDiffAdditionTest,
	"Cortex.Frontend.DiffView.LineDiff.Addition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLineDiffAdditionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString Original = TEXT("line1\nline2\nline3");
	const FString Current = TEXT("line1\nline2\nnew line\nline3");

	TArray<FCortexDiffLine> Lines = CortexDiffUtils::ComputeLineDiff(Original, Current);

	// Should have: 2 unchanged, 1 added, 1 unchanged
	int32 AddedCount = 0;
	int32 UnchangedCount = 0;
	for (const auto& Line : Lines)
	{
		if (Line.Type == ECortexDiffLineType::Added) ++AddedCount;
		if (Line.Type == ECortexDiffLineType::Unchanged) ++UnchangedCount;
	}
	TestEqual(TEXT("Should have 1 added line"), AddedCount, 1);
	TestEqual(TEXT("Should have 3 unchanged lines"), UnchangedCount, 3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexLineDiffRemovalTest,
	"Cortex.Frontend.DiffView.LineDiff.Removal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLineDiffRemovalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString Original = TEXT("line1\nline2\nline3");
	const FString Current = TEXT("line1\nline3");

	TArray<FCortexDiffLine> Lines = CortexDiffUtils::ComputeLineDiff(Original, Current);

	int32 RemovedCount = 0;
	for (const auto& Line : Lines)
	{
		if (Line.Type == ECortexDiffLineType::Removed) ++RemovedCount;
	}
	TestEqual(TEXT("Should have 1 removed line"), RemovedCount, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexLineDiffReplacementTest,
	"Cortex.Frontend.DiffView.LineDiff.Replacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLineDiffReplacementTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString Original = TEXT("line1\nold line\nline3");
	const FString Current = TEXT("line1\nnew line\nline3");

	TArray<FCortexDiffLine> Lines = CortexDiffUtils::ComputeLineDiff(Original, Current);

	int32 AddedCount = 0;
	int32 RemovedCount = 0;
	for (const auto& Line : Lines)
	{
		if (Line.Type == ECortexDiffLineType::Added) ++AddedCount;
		if (Line.Type == ECortexDiffLineType::Removed) ++RemovedCount;
	}
	TestEqual(TEXT("Should have 1 removed line"), RemovedCount, 1);
	TestEqual(TEXT("Should have 1 added line"), AddedCount, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexLineDiffEmptyOriginalTest,
	"Cortex.Frontend.DiffView.LineDiff.EmptyOriginal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLineDiffEmptyOriginalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString Original = TEXT("");
	const FString Current = TEXT("new line1\nnew line2");

	TArray<FCortexDiffLine> Lines = CortexDiffUtils::ComputeLineDiff(Original, Current);

	int32 AddedCount = 0;
	for (const auto& Line : Lines)
	{
		if (Line.Type == ECortexDiffLineType::Added) ++AddedCount;
	}
	TestEqual(TEXT("All lines should be added"), AddedCount, 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexLineDiffIdenticalTest,
	"Cortex.Frontend.DiffView.LineDiff.Identical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLineDiffIdenticalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString Text = TEXT("line1\nline2\nline3");

	TArray<FCortexDiffLine> Lines = CortexDiffUtils::ComputeLineDiff(Text, Text);

	for (const auto& Line : Lines)
	{
		TestEqual(TEXT("All lines should be unchanged"),
			static_cast<int32>(Line.Type), static_cast<int32>(ECortexDiffLineType::Unchanged));
	}
	TestEqual(TEXT("Should have 3 lines"), Lines.Num(), 3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffHunkContextTest,
	"Cortex.Frontend.DiffView.Hunk.ContextLines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffHunkContextTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// 10 unchanged lines, then 1 added, then 10 unchanged
	FString Original;
	FString Current;
	for (int32 i = 0; i < 10; ++i)
	{
		Original += FString::Printf(TEXT("line%d\n"), i);
		Current += FString::Printf(TEXT("line%d\n"), i);
	}
	Current += TEXT("inserted\n");
	for (int32 i = 10; i < 20; ++i)
	{
		Original += FString::Printf(TEXT("line%d\n"), i);
		Current += FString::Printf(TEXT("line%d\n"), i);
	}

	TArray<FCortexDiffHunk> Hunks = CortexDiffUtils::BuildHunks(
		CortexDiffUtils::ComputeLineDiff(Original, Current), 3);

	TestEqual(TEXT("Should produce 1 hunk"), Hunks.Num(), 1);
	// Hunk should have 3 context before + 1 added + 3 context after = 7 visible lines
	TestEqual(TEXT("Hunk should have 7 visible lines"), Hunks[0].Lines.Num(), 7);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffMultiHunkGapTest,
	"Cortex.Frontend.DiffView.Hunk.MultiHunkGaps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffMultiHunkGapTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Build two changes far apart so they produce separate hunks.
	// 10 unchanged, 1 changed, 10 unchanged, 1 changed, 10 unchanged
	FString Original;
	FString Current;
	for (int32 i = 0; i < 10; ++i)
	{
		Original += FString::Printf(TEXT("line%d\n"), i);
		Current  += FString::Printf(TEXT("line%d\n"), i);
	}
	Original += TEXT("old-a\n");
	Current  += TEXT("new-a\n");
	for (int32 i = 11; i < 21; ++i)
	{
		Original += FString::Printf(TEXT("line%d\n"), i);
		Current  += FString::Printf(TEXT("line%d\n"), i);
	}
	Original += TEXT("old-b\n");
	Current  += TEXT("new-b\n");
	for (int32 i = 22; i < 32; ++i)
	{
		Original += FString::Printf(TEXT("line%d\n"), i);
		Current  += FString::Printf(TEXT("line%d\n"), i);
	}

	TArray<FCortexDiffLine> DiffLines = CortexDiffUtils::ComputeLineDiff(Original, Current);
	TArray<FCortexDiffHunk> Hunks = CortexDiffUtils::BuildHunks(DiffLines, 3);

	TestEqual(TEXT("Should produce 2 separate hunks"), Hunks.Num(), 2);

	// First hunk: context [7..10] change [10..12] context [12..14] = 7 lines
	// (indices 7,8,9 context before + removed + added + 10,11,12 context after)
	TestEqual(TEXT("First hunk visible lines"), Hunks[0].Lines.Num(), 7);

	// CollapsedLinesBefore for hunk 0 = 7 (lines 0..6 hidden before it)
	TestEqual(TEXT("First hunk CollapsedLinesBefore"), Hunks[0].CollapsedLinesBefore, 7);

	// CollapsedLinesBefore for hunk 1 must be > first hunk's end (not reset to 0)
	const int32 Hunk0End = Hunks[0].CollapsedLinesBefore + Hunks[0].Lines.Num();
	TestTrue(TEXT("Second hunk starts after first hunk ends"),
		Hunks[1].CollapsedLinesBefore > Hunk0End);

	// The gap between hunks is [Hunk0End, Hunk1.CollapsedLinesBefore)
	const int32 InterHunkGap = Hunks[1].CollapsedLinesBefore - Hunk0End;
	TestTrue(TEXT("Inter-hunk gap should be > 0"), InterHunkGap > 0);

	return true;
}
