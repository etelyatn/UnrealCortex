// Source/CortexFrontend/Private/Widgets/SCortexQAToolbar.cpp
#include "Widgets/SCortexQAToolbar.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Text/STextBlock.h"

void SCortexQAToolbar::Construct(const FArguments& InArgs)
{
    OnRecordConfirmed = InArgs._OnRecordConfirmed;
    OnStop = InArgs._OnStop;
    OnStopAndReplay = InArgs._OnStopAndReplay;
    OnCancelReplay = InArgs._OnCancelReplay;

    ChildSlot
    [
        SNew(SHorizontalBox)

        // Record / Name Input / Stop buttons (3-state switcher)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.f, 0.f)
        [
            SAssignNew(ModeSwitcher, SWidgetSwitcher)

            // Index 0: Record button (idle state)
            + SWidgetSwitcher::Slot()
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Record")))
                .OnClicked_Lambda([this]()
                {
                    OnRecordButtonClicked();
                    return FReply::Handled();
                })
            ]

            // Index 1: Name input (pre-record confirmation)
            + SWidgetSwitcher::Slot()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(0.f, 0.f, 4.f, 0.f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("Name:")))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SAssignNew(NameInput, SEditableTextBox)
                    .MinDesiredWidth(200.f)
                    .OnTextCommitted(FOnTextCommitted::CreateSP(this, &SCortexQAToolbar::OnNameConfirmed))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(4.f, 0.f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Start")))
                    .OnClicked_Lambda([this]()
                    {
                        if (NameInput.IsValid() && !NameInput->GetText().IsEmpty())
                        {
                            bShowingNameInput = false;
                            OnRecordConfirmed.ExecuteIfBound(NameInput->GetText().ToString());
                        }
                        return FReply::Handled();
                    })
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(4.f, 0.f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Cancel")))
                    .OnClicked_Lambda([this]()
                    {
                        bShowingNameInput = false;
                        ModeSwitcher->SetActiveWidgetIndex(0);
                        return FReply::Handled();
                    })
                ]
            ]

            // Index 2: Stop buttons (recording state)
            + SWidgetSwitcher::Slot()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Stop & Replay")))
                    .OnClicked_Lambda([this]()
                    {
                        OnStopAndReplay.ExecuteIfBound();
                        return FReply::Handled();
                    })
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(4.f, 0.f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Stop")))
                    .OnClicked_Lambda([this]()
                    {
                        OnStop.ExecuteIfBound();
                        return FReply::Handled();
                    })
                ]
            ]

            // Index 3: Cancel replay button (replaying state)
            + SWidgetSwitcher::Slot()
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Cancel Replay")))
                .OnClicked_Lambda([this]()
                {
                    OnCancelReplay.ExecuteIfBound();
                    return FReply::Handled();
                })
            ]
        ]

        // Recording ticker
        + SHorizontalBox::Slot()
        .FillWidth(1.f)
        .Padding(8.f, 0.f)
        .VAlign(VAlign_Center)
        [
            SAssignNew(TickerText, STextBlock)
            .Text(FText::GetEmpty())
        ]

        // F9 hint (visible during recording)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.f, 0.f)
        .VAlign(VAlign_Center)
        [
            SNew(STextBlock)
            .Text(FText::FromString(TEXT("F9: Add assertion")))
            .Visibility_Lambda([this]()
            {
                return bIsRecording ? EVisibility::Visible : EVisibility::Collapsed;
            })
        ]

        // PIE status
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.f, 0.f)
        .VAlign(VAlign_Center)
        [
            SAssignNew(StatusText, STextBlock)
            .Text(FText::FromString(TEXT("Idle")))
        ]
    ];
}

void SCortexQAToolbar::OnRecordButtonClicked()
{
    // Show name input with sensible default
    bShowingNameInput = true;
    const FString DefaultName = FString::Printf(TEXT("Recording - %s"),
        *FDateTime::Now().ToString(TEXT("%b %d")));
    if (NameInput.IsValid())
    {
        NameInput->SetText(FText::FromString(DefaultName));
        FSlateApplication::Get().SetKeyboardFocus(NameInput);
    }
    if (ModeSwitcher.IsValid())
    {
        ModeSwitcher->SetActiveWidgetIndex(1);
    }
}

void SCortexQAToolbar::OnNameConfirmed(const FText& Text, ETextCommit::Type CommitType)
{
    if (CommitType == ETextCommit::OnEnter && !Text.IsEmpty())
    {
        bShowingNameInput = false;
        OnRecordConfirmed.ExecuteIfBound(Text.ToString());
    }
}

void SCortexQAToolbar::SetRecording(bool bRecording)
{
    bIsRecording = bRecording;
    bShowingNameInput = false;

    if (ModeSwitcher.IsValid())
    {
        ModeSwitcher->SetActiveWidgetIndex(bRecording ? 2 : 0);
    }

    if (StatusText.IsValid())
    {
        StatusText->SetText(FText::FromString(bRecording ? TEXT("Recording...") : TEXT("Idle")));
    }

    if (!bRecording)
    {
        RecentSteps.Empty();
        if (TickerText.IsValid())
        {
            TickerText->SetText(FText::GetEmpty());
        }
    }
}

void SCortexQAToolbar::SetReplaying(bool bReplaying)
{
    bIsReplaying = bReplaying;

    if (ModeSwitcher.IsValid())
    {
        ModeSwitcher->SetActiveWidgetIndex(bReplaying ? 3 : 0);
    }

    if (StatusText.IsValid())
    {
        StatusText->SetText(FText::FromString(bReplaying ? TEXT("Replaying...") : TEXT("Idle")));
    }
}

void SCortexQAToolbar::AddRecordingStep(const FString& StepType, const FString& Target)
{
    FString Entry = Target.IsEmpty() ? StepType : FString::Printf(TEXT("%s %s"), *StepType, *Target);
    RecentSteps.Add(Entry);

    // Keep last 5
    while (RecentSteps.Num() > 5)
    {
        RecentSteps.RemoveAt(0);
    }

    // Update ticker display
    if (TickerText.IsValid())
    {
        TickerText->SetText(FText::FromString(FString::Join(RecentSteps, TEXT(" | "))));
    }
}

void SCortexQAToolbar::SetPIEStatus(const FString& Status)
{
    if (StatusText.IsValid())
    {
        StatusText->SetText(FText::FromString(Status));
    }
}
