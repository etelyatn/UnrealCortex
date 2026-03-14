#include "Widgets/SCortexCodeBlock.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Rendering/CortexSyntaxHighlighter.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SCortexCodeBlock::Construct(const FArguments& InArgs)
{
    CodeContent = InArgs._Code;

    ChildSlot
    [
        SNew(SBorder)
        .BorderBackgroundColor(FLinearColor(0.05f, 0.05f, 0.08f, 1.0f))
        .Padding(0.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .Padding(8.0f, 4.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(InArgs._Language))
                    .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(4.0f, 2.0f)
                [
                    SNew(SButton)
                    .OnClicked(this, &SCortexCodeBlock::OnCopyClicked)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(TEXT("Copy")))
                    ]
                ]
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f, 4.0f)
            [
                SNew(SMultiLineEditableText)
                .Text(FText::FromString(CodeContent))
                .IsReadOnly(true)
                .AutoWrapText(false)
                .Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
            ]
        ]
    ];
}

FReply SCortexCodeBlock::OnCopyClicked()
{
    FPlatformApplicationMisc::ClipboardCopy(*CodeContent);
    return FReply::Handled();
}
