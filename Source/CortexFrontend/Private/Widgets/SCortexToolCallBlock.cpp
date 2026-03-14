#include "Widgets/SCortexToolCallBlock.h"

#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SCortexToolCallBlock::Construct(const FArguments& InArgs)
{
    ToolCallList = InArgs._ToolCalls;

    ChildSlot
    [
        SAssignNew(ContentBox, SVerticalBox)
    ];

    RebuildContent();
}

FReply SCortexToolCallBlock::OnToggleExpand()
{
    bIsExpanded = !bIsExpanded;
    RebuildContent();
    return FReply::Handled();
}

void SCortexToolCallBlock::RebuildContent()
{
    ContentBox->ClearChildren();

    int32 TotalMs = 0;
    for (const TSharedPtr<FCortexChatEntry>& Call : ToolCallList)
    {
        TotalMs += Call->DurationMs;
    }

    const FString Arrow = bIsExpanded ? TEXT("\u25bc") : TEXT("\u25b6");
    const FString HeaderText = FString::Printf(TEXT("%s %d tool call%s  %dms"),
        *Arrow,
        ToolCallList.Num(),
        ToolCallList.Num() == 1 ? TEXT("") : TEXT("s"),
        TotalMs);

    // Header row — clickable toggle
    ContentBox->AddSlot()
    .AutoHeight()
    [
        SNew(SButton)
        .ButtonStyle(FCoreStyle::Get(), "NoBorder")
        .OnClicked(this, &SCortexToolCallBlock::OnToggleExpand)
        [
            SNew(STextBlock)
            .Text(FText::FromString(HeaderText))
            .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.7f, 0.9f)))
        ]
    ];

    if (bIsExpanded)
    {
        for (const TSharedPtr<FCortexChatEntry>& Call : ToolCallList)
        {
            const FString StatusIcon = Call->bIsToolComplete ? TEXT("\u2713") : TEXT("\u2026");
            const FString RowText = FString::Printf(TEXT("  %s %s  %dms"),
                *StatusIcon,
                *Call->ToolName,
                Call->DurationMs);

            FString ResultSummary;
            if (!Call->ToolResult.IsEmpty())
            {
                ResultSummary = Call->ToolResult.Left(80);
                if (Call->ToolResult.Len() > 80)
                {
                    ResultSummary += TEXT("...");
                }
            }

            ContentBox->AddSlot()
            .AutoHeight()
            .Padding(8.0f, 1.0f, 0.0f, 1.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(RowText))
                .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
                .ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
            ];

            if (!ResultSummary.IsEmpty())
            {
                ContentBox->AddSlot()
                .AutoHeight()
                .Padding(24.0f, 0.0f, 0.0f, 2.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(ResultSummary))
                    .Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
                    .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
                    .AutoWrapText(true)
                ];
            }
        }
    }
}
