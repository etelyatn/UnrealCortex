#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SScrollBox;

// ── Diff line types ──
enum class ECortexDiffLineType : uint8
{
	Unchanged,
	Added,
	Removed
};

// ── Single diff line ──
struct FCortexDiffLine
{
	ECortexDiffLineType Type;
	FString Text;
	int32 OriginalLineNum = -1;  // -1 if not present in original
	int32 CurrentLineNum = -1;   // -1 if not present in current
};

// ── A hunk: group of diff lines with context ──
struct FCortexDiffHunk
{
	TArray<FCortexDiffLine> Lines;
	int32 CollapsedLinesBefore = 0;  // lines hidden before this hunk
};

// ── Diff computation utilities (static, testable without widgets) ──
namespace CortexDiffUtils
{
	/** Compute a line-level diff between two texts. Uses a simple LCS-based algorithm. */
	TArray<FCortexDiffLine> ComputeLineDiff(const FString& Original, const FString& Current);

	/** Group diff lines into hunks with N context lines around changes. */
	TArray<FCortexDiffHunk> BuildHunks(const TArray<FCortexDiffLine>& DiffLines, int32 ContextLines = 3);
}

// ── Diff view widget ──
class SCortexInheritedDiffView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexInheritedDiffView) {}
		SLATE_ARGUMENT(FString, OriginalText)
		SLATE_ARGUMENT(FString, CurrentText)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Called by SCortexCodeCanvas on document change. Recomputes diff and rebuilds widget tree. */
	void SetCurrentText(const FString& NewCurrentText);

private:
	void RebuildDiffView();
	TSharedRef<SWidget> MakeDiffLineWidget(const FCortexDiffLine& Line);
	TSharedRef<SWidget> MakeCollapsedSection(int32 HunkIndex, int32 LineCount);

	FString OriginalText;
	FString CurrentText;
	TSharedPtr<SScrollBox> ScrollBox;
	TSet<int32> ExpandedHunkIndices;
};
