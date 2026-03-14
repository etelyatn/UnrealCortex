#include "Widgets/SCortexChatToolbar.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

// Context calculation helpers (moved from SCortexContextBar)
namespace CortexContextUtils
{
    static const int64 DefaultContextLimit = 200000;

    static int64 GetContextLimit(const FString& ModelId)
    {
        // All Claude models currently have 200K context
        return DefaultContextLimit;
    }

    static float CalculatePercentage(int64 Used, int64 Max)
    {
        if (Max <= 0) return 0.0f;
        return static_cast<float>(Used) / static_cast<float>(Max) * 100.0f;
    }

    static FLinearColor GetContextColor(float Percentage)
    {
        if (Percentage >= 80.0f)
        {
            return FLinearColor(0.76f, 0.18f, 0.18f, 1.0f); // Red
        }
        if (Percentage >= 60.0f)
        {
            return FLinearColor(0.77f, 0.63f, 0.20f, 1.0f); // Yellow
        }
        return FLinearColor(0.05f, 0.51f, 0.75f, 1.0f); // Blue
    }
}

void SCortexChatToolbar::Construct(const FArguments& InArgs)
{
    OnNewChat = InArgs._OnNewChat;
    SessionWeak = InArgs._Session;

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
            .ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888")))))
        ]
        // Context indicator (right side)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(4.0f, 4.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 4.0f, 0.0f)
            [
                SNew(SBox)
                .WidthOverride(8.0f)
                .HeightOverride(8.0f)
                [
                    SAssignNew(ContextColorBox, SBorder)
                    .BorderBackgroundColor(CortexContextUtils::GetContextColor(0.0f))
                ]
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            [
                SAssignNew(ContextLabel, STextBlock)
                .Text(FText::FromString(TEXT("0 / 200k")))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                .ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888")))))
            ]
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

    // Subscribe to token updates
    if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
    {
        TWeakPtr<SCortexChatToolbar> SelfWeak = SharedThis(this);
        TokenUsageHandle = Session->OnTokenUsageUpdated.AddLambda([SelfWeak]()
        {
            if (TSharedPtr<SCortexChatToolbar> Self = SelfWeak.Pin())
            {
                Self->OnTokenUsageUpdated();
            }
        });
    }
}

SCortexChatToolbar::~SCortexChatToolbar()
{
    if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
    {
        Session->OnTokenUsageUpdated.Remove(TokenUsageHandle);
    }
}

void SCortexChatToolbar::SetSessionId(const FString& SessionId)
{
    if (SessionIdText.IsValid())
    {
        SessionIdText->SetText(FText::FromString(SessionId));
    }
}

void SCortexChatToolbar::OnTokenUsageUpdated()
{
    TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin();
    if (!Session.IsValid()) return;

    const int64 Used = Session->GetConversationContextTokens();
    const int64 Max = CortexContextUtils::GetContextLimit(Session->GetModelId());
    const float Percentage = CortexContextUtils::CalculatePercentage(Used, Max);

    if (ContextColorBox.IsValid())
    {
        ContextColorBox->SetBorderBackgroundColor(CortexContextUtils::GetContextColor(Percentage));
    }

    if (ContextLabel.IsValid())
    {
        const FString Label = FString::Printf(TEXT("%lldk / %lldk"), Used / 1000, Max / 1000);
        ContextLabel->SetText(FText::FromString(Label));
    }
}
