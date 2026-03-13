#include "Widgets/SCortexChatMessage.h"

#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SCortexChatMessage::Construct(const FArguments& InArgs)
{
    const FLinearColor BackgroundColor = InArgs._IsUser ? FLinearColor(0.25f, 0.25f, 0.45f, 0.15f) : FLinearColor(0.1f, 0.1f, 0.1f, 0.3f);

    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 2.0f)
        [
            SNew(STextBlock)
            .Text(FText::FromString(InArgs._IsUser ? TEXT("You") : TEXT("Claude")))
            .ColorAndOpacity(FSlateColor(FLinearColor(0.58f, 0.63f, 0.73f)))
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SBorder)
            .BorderBackgroundColor(BackgroundColor)
            .Padding(FMargin(12.0f, 8.0f))
            [
                SAssignNew(MessageText, STextBlock)
                .Text(FText::FromString(InArgs._Message))
                .AutoWrapText(true)
            ]
        ]
    ];
}

void SCortexChatMessage::SetText(const FString& NewText)
{
    if (MessageText.IsValid())
    {
        MessageText->SetText(FText::FromString(NewText));
    }
}
