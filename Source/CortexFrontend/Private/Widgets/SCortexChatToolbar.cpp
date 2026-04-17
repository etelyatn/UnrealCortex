#include "Widgets/SCortexChatToolbar.h"

#include "CortexFrontendSettings.h"
#include "Providers/CortexProviderRegistry.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

// Context calculation helpers (moved from SCortexContextBar)
namespace
{
    float CalculatePercentage(int64 Used, int64 Max)
    {
        if (Max <= 0) return 0.0f;
        return static_cast<float>(Used) / static_cast<float>(Max) * 100.0f;
    }

    FString FormatTokenCount(int64 Tokens)
    {
        return (Tokens < 1000)
            ? FString::Printf(TEXT("%lld"), Tokens)
            : FString::Printf(TEXT("%lldk"), Tokens / 1000);
    }

    FLinearColor GetContextColor(float Percentage)
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
        // Active model label
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(4.0f, 4.0f)
        [
            SAssignNew(ModelLabel, STextBlock)
            .Text(FText::FromString(TEXT("")))
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
            .ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("6a9955")))))
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
                    .BorderBackgroundColor(GetContextColor(0.0f))
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

        StateChangedHandle = Session->OnStateChanged.AddLambda([SelfWeak](const FCortexSessionStateChange& Change)
        {
            if (TSharedPtr<SCortexChatToolbar> Self = SelfWeak.Pin())
            {
                Self->OnSessionStateChanged(Change);
            }
        });

        RefreshModelLabel();
        RefreshContextIndicator();
    }
}

SCortexChatToolbar::~SCortexChatToolbar()
{
    if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
    {
        Session->OnTokenUsageUpdated.Remove(TokenUsageHandle);
        Session->OnStateChanged.Remove(StateChangedHandle);
    }
}

void SCortexChatToolbar::SetSessionId(const FString& SessionId)
{
    if (SessionIdText.IsValid())
    {
        SessionIdText->SetText(FText::FromString(SessionId));
    }
}

void SCortexChatToolbar::SetModelLabel(const FString& ModelId)
{
    if (ModelLabel.IsValid() && !ModelId.IsEmpty())
    {
        ModelLabel->SetText(FText::FromString(ModelId));
    }
}

void SCortexChatToolbar::RefreshModelLabel()
{
    TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin();
    if (!Session.IsValid() || !ModelLabel.IsValid())
    {
        return;
    }

    const FCortexProviderDefinition& ProviderDefinition =
        FCortexProviderRegistry::ResolveDefinition(Session->GetProviderId().ToString());
    const FString Label = FCortexFrontendSettings::FormatModelLabel(
        Session->GetResolvedOptions().ProviderDisplayName,
        Session->GetModelId(),
        Session->GetResolvedOptions().EffortLevel,
        ProviderDefinition.DefaultEffortLevel);
    ModelLabel->SetText(FText::FromString(Label));
}

void SCortexChatToolbar::RefreshContextIndicator()
{
    TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin();
    if (!Session.IsValid())
    {
        return;
    }

    const int64 Used = Session->GetConversationContextTokens();
    const int64 Max = Session->GetContextLimitTokens();
    const float Percentage = CalculatePercentage(Used, Max);

    if (ContextColorBox.IsValid())
    {
        ContextColorBox->SetBorderBackgroundColor(GetContextColor(Percentage));
    }

    if (ContextLabel.IsValid())
    {
        ContextLabel->SetText(FText::FromString(
            FormatTokenCount(Used) + TEXT(" / ") + FormatTokenCount(Max)));
    }
}

void SCortexChatToolbar::OnTokenUsageUpdated()
{
    RefreshContextIndicator();
}

void SCortexChatToolbar::OnSessionStateChanged(const FCortexSessionStateChange& Change)
{
    const bool bDisconnected = Change.NewState == ECortexSessionState::Inactive
        || Change.NewState == ECortexSessionState::Terminated;

    if (bDisconnected && ModelLabel.IsValid())
    {
        // Hide model label when disconnected — it shows stale data from the previous session
        ModelLabel->SetText(FText::FromString(TEXT("")));
        return;
    }

    RefreshModelLabel();
}
