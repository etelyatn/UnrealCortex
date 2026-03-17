#include "Widgets/SCortexDiffBlock.h"

#include "HAL/PlatformApplicationMisc.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace CortexDiffColors
{
    // Catppuccin Mocha-aligned diff colors
    static FLinearColor RemovedBg()   { return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("3b1219"))); }
    static FLinearColor RemovedText() { return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("f38ba8"))); }
    static FLinearColor AddedBg()     { return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("13301a"))); }
    static FLinearColor AddedText()   { return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("a6e3a1"))); }
    static FLinearColor Overlay1()    { return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("6c7086"))); }
    static FLinearColor Base()        { return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("1e1e2e"))); }
}

void SCortexDiffBlock::Construct(const FArguments& InArgs)
{
    Pairs = InArgs._Pairs;
    const FString Target = InArgs._Target;

    // Build diff lines
    TSharedRef<SVerticalBox> DiffLines = SNew(SVerticalBox);

    for (int32 i = 0; i < Pairs.Num(); ++i)
    {
        if (i > 0)
        {
            DiffLines->AddSlot()
            .AutoHeight()
            [
                MakeSeparator()
            ];
        }

        // Search (removed) lines — preserve intentional blank lines,
        // but trim the trailing empty entry from the parser's trailing \n
        TArray<FString> SearchLines;
        Pairs[i].SearchText.ParseIntoArray(SearchLines, TEXT("\n"), false);
        if (SearchLines.Num() > 0 && SearchLines.Last().IsEmpty())
        {
            SearchLines.Pop();
        }
        for (const FString& Line : SearchLines)
        {
            DiffLines->AddSlot()
            .AutoHeight()
            [
                MakeDiffLine(Line, /*bIsRemoval=*/ true)
            ];
        }

        // Replace (added) lines — same trailing-empty trim
        TArray<FString> ReplaceLines;
        Pairs[i].ReplaceText.ParseIntoArray(ReplaceLines, TEXT("\n"), false);
        if (ReplaceLines.Num() > 0 && ReplaceLines.Last().IsEmpty())
        {
            ReplaceLines.Pop();
        }
        for (const FString& Line : ReplaceLines)
        {
            DiffLines->AddSlot()
            .AutoHeight()
            [
                MakeDiffLine(Line, /*bIsRemoval=*/ false)
            ];
        }
    }

    ChildSlot
    [
        SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
        .BorderBackgroundColor(CortexDiffColors::Base())
        .Padding(0.0f)
        [
            SNew(SVerticalBox)

            // Header
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f, 4.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(GetHeaderLabel(Target)))
                .ColorAndOpacity(FSlateColor(CortexDiffColors::Overlay1()))
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
            ]

            // Diff content (SBox caps height; SScrollBox enables scrolling within)
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SBox)
                .MaxDesiredHeight(400.0f)
                [
                    SNew(SScrollBox)
                    + SScrollBox::Slot()
                    [
                        DiffLines
                    ]
                ]
            ]

            // Footer: Apply + Copy + Revert
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f, 4.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0, 0, 4, 0)
                [
                    SNew(SButton)
                    .Text(FText::FromString(GetApplyLabel(Target)))
                    .OnClicked(InArgs._OnApply)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0, 0, 4, 0)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Copy")))
                    .OnClicked(this, &SCortexDiffBlock::OnCopyClicked)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Revert")))
                    .OnClicked(InArgs._OnRevert)
                    .Visibility_Lambda([IsRevertVisible = InArgs._IsRevertVisible]()
                    {
                        return IsRevertVisible.Get(false) ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                ]
            ]
        ]
    ];
}

TSharedRef<SWidget> SCortexDiffBlock::MakeDiffLine(const FString& Text, bool bIsRemoval)
{
    const FString Prefix = bIsRemoval ? TEXT("- ") : TEXT("+ ");
    const FLinearColor BgColor = bIsRemoval ? CortexDiffColors::RemovedBg() : CortexDiffColors::AddedBg();
    const FLinearColor TextColor = bIsRemoval ? CortexDiffColors::RemovedText() : CortexDiffColors::AddedText();

    return SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
        .BorderBackgroundColor(BgColor)
        .Padding(FMargin(8.0f, 1.0f))
        [
            SNew(STextBlock)
            .Text(FText::FromString(Prefix + Text))
            .ColorAndOpacity(FSlateColor(TextColor))
            .Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
        ];
}

TSharedRef<SWidget> SCortexDiffBlock::MakeSeparator()
{
    return SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
        .BorderBackgroundColor(FLinearColor::Transparent)
        .Padding(FMargin(0.0f, 2.0f))
        .HAlign(HAlign_Center)
        [
            SNew(STextBlock)
            .Text(FText::FromString(FString(TEXT("\u00B7\u00B7\u00B7"))))  // middle dots
            .ColorAndOpacity(FSlateColor(CortexDiffColors::Overlay1()))
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
        ];
}

FString SCortexDiffBlock::GetHeaderLabel(const FString& Target) const
{
    if (Target == TEXT("header"))
    {
        return TEXT("Changes in .h");
    }
    if (Target == TEXT("implementation"))
    {
        return TEXT("Changes in .cpp");
    }
    if (Target == TEXT("snippet"))
    {
        return TEXT("Changes in snippet");
    }
    return TEXT("Changes");
}

FString SCortexDiffBlock::GetApplyLabel(const FString& Target) const
{
    if (Target == TEXT("header"))
    {
        return TEXT("Apply to .h");
    }
    if (Target == TEXT("implementation"))
    {
        return TEXT("Apply to .cpp");
    }
    if (Target == TEXT("snippet"))
    {
        return TEXT("Apply Snippet");
    }
    return TEXT("Apply");
}

FReply SCortexDiffBlock::OnCopyClicked()
{
    // Copy all REPLACE texts concatenated with \n\n separators
    FString CopyText;
    for (int32 i = 0; i < Pairs.Num(); ++i)
    {
        if (i > 0)
        {
            CopyText += TEXT("\n\n");
        }
        CopyText += Pairs[i].ReplaceText;
    }
    FPlatformApplicationMisc::ClipboardCopy(*CopyText);
    return FReply::Handled();
}
