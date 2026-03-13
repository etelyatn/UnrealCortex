#include "Widgets/SCortexToolCallBlock.h"

#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Text/STextBlock.h"

void SCortexToolCallBlock::Construct(const FArguments& InArgs)
{
    FString HeaderText = InArgs._ToolName;
    if (InArgs._bIsComplete)
    {
        HeaderText += FString::Printf(TEXT("  %dms"), InArgs._DurationMs);
    }
    else
    {
        HeaderText += TEXT("  running...");
    }

    FString BodyText;
    if (!InArgs._ToolInput.IsEmpty())
    {
        BodyText += FString::Printf(TEXT("Input: %s"), *InArgs._ToolInput);
    }
    if (!InArgs._ToolResult.IsEmpty())
    {
        if (!BodyText.IsEmpty())
        {
            BodyText += TEXT("\n\n");
        }
        BodyText += FString::Printf(TEXT("Result: %s"), *InArgs._ToolResult);
    }

    ChildSlot
    [
        SNew(SExpandableArea)
        .InitiallyCollapsed(true)
        .HeaderContent()
        [
            SNew(STextBlock)
            .Text(FText::FromString(HeaderText))
            .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
        ]
        .BodyContent()
        [
            SNew(STextBlock)
            .Text(FText::FromString(BodyText))
            .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
            .AutoWrapText(true)
        ]
    ];
}

void SCortexToolCallBlock::SetResult(const FString& Result, int32 DurationMs)
{
    (void)Result;
    (void)DurationMs;
}
