#include "Widgets/SCortexInheritedDiffView.h"

#include "CortexFrontendModule.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	// Catppuccin Mocha diff colors (matching SCortexDiffBlock)
	const FLinearColor AddedBg(0.075f, 0.188f, 0.102f);     // #13301a
	const FLinearColor AddedText(0.651f, 0.890f, 0.631f);   // #a6e3a1
	const FLinearColor RemovedBg(0.231f, 0.071f, 0.098f);   // #3b1219
	const FLinearColor RemovedText(0.953f, 0.545f, 0.659f); // #f38ba8
	const FLinearColor UnchangedText(0.7f, 0.7f, 0.7f);
	const FLinearColor LineNumColor(0.4f, 0.4f, 0.4f);
	const FLinearColor CollapsedBg(0.15f, 0.15f, 0.18f);
	const FLinearColor CollapsedText(0.5f, 0.5f, 0.6f);
}

// ── LCS-based line diff ──

TArray<FCortexDiffLine> CortexDiffUtils::ComputeLineDiff(const FString& Original, const FString& Current)
{
	TArray<FString> OldLines;
	TArray<FString> NewLines;
	Original.ParseIntoArrayLines(OldLines, false);
	Current.ParseIntoArrayLines(NewLines, false);

	const int32 M = OldLines.Num();
	const int32 N = NewLines.Num();

	// Build LCS table
	TArray<TArray<int32>> LCS;
	LCS.SetNum(M + 1);
	for (int32 i = 0; i <= M; ++i)
	{
		LCS[i].SetNumZeroed(N + 1);
	}
	for (int32 i = M - 1; i >= 0; --i)
	{
		for (int32 j = N - 1; j >= 0; --j)
		{
			if (OldLines[i] == NewLines[j])
			{
				LCS[i][j] = LCS[i + 1][j + 1] + 1;
			}
			else
			{
				LCS[i][j] = FMath::Max(LCS[i + 1][j], LCS[i][j + 1]);
			}
		}
	}

	// Backtrack to produce diff
	TArray<FCortexDiffLine> Result;
	int32 i = 0, j = 0;
	int32 OldLineNum = 1, NewLineNum = 1;
	while (i < M || j < N)
	{
		if (i < M && j < N && OldLines[i] == NewLines[j])
		{
			FCortexDiffLine Line;
			Line.Type = ECortexDiffLineType::Unchanged;
			Line.Text = OldLines[i];
			Line.OriginalLineNum = OldLineNum++;
			Line.CurrentLineNum = NewLineNum++;
			Result.Add(Line);
			++i; ++j;
		}
		else if (j < N && (i >= M || LCS[i][j + 1] >= LCS[i + 1][j]))
		{
			FCortexDiffLine Line;
			Line.Type = ECortexDiffLineType::Added;
			Line.Text = NewLines[j];
			Line.CurrentLineNum = NewLineNum++;
			Result.Add(Line);
			++j;
		}
		else
		{
			FCortexDiffLine Line;
			Line.Type = ECortexDiffLineType::Removed;
			Line.Text = OldLines[i];
			Line.OriginalLineNum = OldLineNum++;
			Result.Add(Line);
			++i;
		}
	}

	return Result;
}

TArray<FCortexDiffHunk> CortexDiffUtils::BuildHunks(const TArray<FCortexDiffLine>& DiffLines, int32 ContextLines)
{
	TArray<FCortexDiffHunk> Hunks;

	// Find indices of changed lines
	TArray<int32> ChangedIndices;
	for (int32 i = 0; i < DiffLines.Num(); ++i)
	{
		if (DiffLines[i].Type != ECortexDiffLineType::Unchanged)
		{
			ChangedIndices.Add(i);
		}
	}

	if (ChangedIndices.IsEmpty())
	{
		return Hunks;
	}

	// Group changes into hunks (merge if context windows overlap)
	int32 HunkStart = FMath::Max(0, ChangedIndices[0] - ContextLines);
	int32 HunkEnd = FMath::Min(DiffLines.Num() - 1, ChangedIndices[0] + ContextLines);

	for (int32 c = 1; c < ChangedIndices.Num(); ++c)
	{
		const int32 NextStart = FMath::Max(0, ChangedIndices[c] - ContextLines);
		const int32 NextEnd = FMath::Min(DiffLines.Num() - 1, ChangedIndices[c] + ContextLines);

		if (NextStart <= HunkEnd + 1)
		{
			// Merge into current hunk
			HunkEnd = NextEnd;
		}
		else
		{
			// Emit current hunk, start new one
			FCortexDiffHunk Hunk;
			Hunk.CollapsedLinesBefore = HunkStart;
			for (int32 ii = HunkStart; ii <= HunkEnd; ++ii)
			{
				Hunk.Lines.Add(DiffLines[ii]);
			}
			Hunks.Add(Hunk);

			HunkStart = NextStart;
			HunkEnd = NextEnd;
		}
	}

	// Emit final hunk
	FCortexDiffHunk Hunk;
	Hunk.CollapsedLinesBefore = HunkStart;
	for (int32 ii = HunkStart; ii <= HunkEnd; ++ii)
	{
		Hunk.Lines.Add(DiffLines[ii]);
	}
	Hunks.Add(Hunk);

	return Hunks;
}

// ── Widget implementation ──

void SCortexInheritedDiffView::Construct(const FArguments& InArgs)
{
	OriginalText = InArgs._OriginalText;
	CurrentText = InArgs._CurrentText;

	ChildSlot
	[
		SAssignNew(ScrollBox, SScrollBox)
	];

	RebuildDiffView();
}

void SCortexInheritedDiffView::SetCurrentText(const FString& NewCurrentText)
{
	CurrentText = NewCurrentText;
	RebuildDiffView();
}

void SCortexInheritedDiffView::RebuildDiffView()
{
	if (!ScrollBox.IsValid())
	{
		return;
	}

	ScrollBox->ClearChildren();

	if (OriginalText.IsEmpty() && CurrentText.IsEmpty())
	{
		return;
	}

	TArray<FCortexDiffLine> DiffLines = CortexDiffUtils::ComputeLineDiff(OriginalText, CurrentText);
	TArray<FCortexDiffHunk> Hunks = CortexDiffUtils::BuildHunks(DiffLines, 3);

	for (int32 HunkIdx = 0; HunkIdx < Hunks.Num(); ++HunkIdx)
	{
		const FCortexDiffHunk& HunkRef = Hunks[HunkIdx];

		// Collapsed section before this hunk
		if (HunkRef.CollapsedLinesBefore > 0 && !ExpandedHunkIndices.Contains(HunkIdx * 2))
		{
			ScrollBox->AddSlot()
			[
				MakeCollapsedSection(HunkIdx * 2, HunkRef.CollapsedLinesBefore)
			];
		}
		else if (HunkRef.CollapsedLinesBefore > 0)
		{
			// Expanded — show all lines from start to hunk start
			for (int32 ii = 0; ii < HunkRef.CollapsedLinesBefore && ii < DiffLines.Num(); ++ii)
			{
				ScrollBox->AddSlot()
				[
					MakeDiffLineWidget(DiffLines[ii])
				];
			}
		}

		// Hunk lines
		for (const FCortexDiffLine& Line : HunkRef.Lines)
		{
			ScrollBox->AddSlot()
			[
				MakeDiffLineWidget(Line)
			];
		}

		// Collapsed section after last hunk
		if (HunkIdx == Hunks.Num() - 1)
		{
			const int32 LastHunkEnd = HunkRef.CollapsedLinesBefore + HunkRef.Lines.Num();
			const int32 RemainingLines = DiffLines.Num() - LastHunkEnd;
			if (RemainingLines > 0 && !ExpandedHunkIndices.Contains(HunkIdx * 2 + 1))
			{
				ScrollBox->AddSlot()
				[
					MakeCollapsedSection(HunkIdx * 2 + 1, RemainingLines)
				];
			}
		}
	}

	// If no hunks (identical files), show collapsed "no changes" indicator
	if (Hunks.IsEmpty() && !DiffLines.IsEmpty())
	{
		ScrollBox->AddSlot()
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("No changes — %d lines identical"), DiffLines.Num())))
			.ColorAndOpacity(FSlateColor(CollapsedText))
			.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
		];
	}
}

TSharedRef<SWidget> SCortexInheritedDiffView::MakeDiffLineWidget(const FCortexDiffLine& Line)
{
	FLinearColor BgColor(0.0f, 0.0f, 0.0f, 0.0f);
	FLinearColor TextColor = UnchangedText;

	switch (Line.Type)
	{
	case ECortexDiffLineType::Added:
		BgColor = AddedBg;
		TextColor = AddedText;
		break;
	case ECortexDiffLineType::Removed:
		BgColor = RemovedBg;
		TextColor = RemovedText;
		break;
	default:
		break;
	}

	const FString OrigNum = (Line.OriginalLineNum >= 0)
		? FString::Printf(TEXT("%4d"), Line.OriginalLineNum) : TEXT("    ");
	const FString CurrNum = (Line.CurrentLineNum >= 0)
		? FString::Printf(TEXT("%4d"), Line.CurrentLineNum) : TEXT("    ");

	const FString Prefix = (Line.Type == ECortexDiffLineType::Added) ? TEXT("+")
		: (Line.Type == ECortexDiffLineType::Removed) ? TEXT("-") : TEXT(" ");

	return SNew(SBorder)
		.BorderBackgroundColor(FSlateColor(BgColor))
		.Padding(FMargin(4, 1))
		[
			SNew(SHorizontalBox)

			// Original line number
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0, 0, 4, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(OrigNum))
				.ColorAndOpacity(FSlateColor(LineNumColor))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			]

			// Current line number
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(CurrNum))
				.ColorAndOpacity(FSlateColor(LineNumColor))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			]

			// +/- prefix and line text
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Prefix + TEXT(" ") + Line.Text))
				.ColorAndOpacity(FSlateColor(TextColor))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			]
		];
}

TSharedRef<SWidget> SCortexInheritedDiffView::MakeCollapsedSection(int32 HunkIndex, int32 LineCount)
{
	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "NoBorder")
		.OnClicked_Lambda([this, HunkIndex]()
		{
			ExpandedHunkIndices.Add(HunkIndex);
			RebuildDiffView();
			return FReply::Handled();
		})
		[
			SNew(SBorder)
			.BorderBackgroundColor(FSlateColor(CollapsedBg))
			.Padding(FMargin(8, 4))
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("... %d lines hidden ..."), LineCount)))
				.ColorAndOpacity(FSlateColor(CollapsedText))
				.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
				.Justification(ETextJustify::Center)
			]
		];
}
