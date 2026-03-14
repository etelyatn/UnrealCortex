#include "Widgets/SCortexContextBar.h"

#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Notifications/SProgressBar.h"

// Context window limits per model prefix (tokens)
static const TMap<FString, int64> GContextLimits = {
    { TEXT("claude-opus"),   200000 },
    { TEXT("claude-sonnet"), 200000 },
    { TEXT("claude-haiku"),  200000 },
};
static const int64 GDefaultContextLimit = 200000;

const TMap<FString, int64>& SCortexContextBar::GetContextLimits()
{
    return GContextLimits;
}

int64 SCortexContextBar::GetContextLimit(const FString& ModelId)
{
    for (const auto& Pair : GContextLimits)
    {
        if (ModelId.StartsWith(Pair.Key))
        {
            return Pair.Value;
        }
    }
    return GDefaultContextLimit;
}

float SCortexContextBar::CalculatePercentage(int64 Used, int64 Max)
{
    if (Max <= 0) return 0.0f;
    return static_cast<float>(Used) / static_cast<float>(Max) * 100.0f;
}

FLinearColor SCortexContextBar::GetContextColor(float Percentage)
{
    if (Percentage >= 80.0f)
    {
        // Red
        return FLinearColor(0.76f, 0.18f, 0.18f, 1.0f);
    }
    if (Percentage >= 60.0f)
    {
        // Yellow
        return FLinearColor(0.77f, 0.63f, 0.20f, 1.0f);
    }
    // Blue
    return FLinearColor(0.05f, 0.51f, 0.75f, 1.0f);
}

void SCortexContextBar::Construct(const FArguments& InArgs)
{
    SessionWeak = InArgs._Session;

    ChildSlot
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .FillWidth(1.0f)
        .VAlign(VAlign_Center)
        .Padding(8.0f, 2.0f)
        [
            SAssignNew(ProgressBar, SProgressBar)
            .Percent(TOptional<float>(0.0f))
            .FillColorAndOpacity(FSlateColor(GetContextColor(0.0f)))
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(4.0f, 2.0f)
        [
            SAssignNew(UsageLabel, STextBlock)
            .Text(FText::FromString(TEXT("0 / 200k")))
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
            .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
        ]
    ];

    if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
    {
        TWeakPtr<SCortexContextBar> SelfWeak = SharedThis(this);
        Session->OnTokenUsageUpdated.AddLambda([SelfWeak]()
        {
            if (TSharedPtr<SCortexContextBar> Self = SelfWeak.Pin())
            {
                Self->OnTokenUsageUpdated();
            }
        });
    }
}

void SCortexContextBar::OnTokenUsageUpdated()
{
    TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin();
    if (!Session.IsValid()) return;

    const int64 Used = Session->GetConversationContextTokens();
    const int64 Max = GetContextLimit(Session->GetModelId());
    const float Percentage = CalculatePercentage(Used, Max);

    if (ProgressBar.IsValid())
    {
        ProgressBar->SetPercent(TOptional<float>(Percentage / 100.0f));
        ProgressBar->SetFillColorAndOpacity(FSlateColor(GetContextColor(Percentage)));
    }

    if (UsageLabel.IsValid())
    {
        const FString Label = FString::Printf(TEXT("%lld / %lldK"),
            Used / 1000, Max / 1000);
        UsageLabel->SetText(FText::FromString(Label));
    }
}
