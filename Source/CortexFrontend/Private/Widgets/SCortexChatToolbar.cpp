#include "Widgets/SCortexChatToolbar.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Styling/AppStyle.h"

void SCortexChatToolbar::Construct(const FArguments& InArgs)
{
    OnNewChat = InArgs._OnNewChat;

    ChildSlot
    [
        SNew(SHorizontalBox)
        // Session ID (left side)
        + SHorizontalBox::Slot()
        .FillWidth(1.0f)
        .VAlign(VAlign_Center)
        .Padding(8.0f, 4.0f)
        [
            SAssignNew(SessionIdText, STextBlock)
            .Text(FText::FromString(TEXT("")))
            .ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f))
        ]
        // New Chat button (right side)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(4.0f)
        [
            SNew(SButton)
            .Text(FText::FromString(TEXT("New Chat")))
            .OnClicked_Lambda([this]() -> FReply
            {
                OnNewChat.ExecuteIfBound();
                return FReply::Handled();
            })
        ]
    ];
}

void SCortexChatToolbar::SetSessionId(const FString& SessionId)
{
    if (SessionIdText.IsValid())
    {
        SessionIdText->SetText(FText::FromString(SessionId));
    }
}
