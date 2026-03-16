// Source/CortexFrontend/Private/Widgets/SCortexQAToolbar.h
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE_OneParam(FOnQARecordConfirmed, const FString& /* SessionName */);
DECLARE_DELEGATE(FOnQAStopClicked);
DECLARE_DELEGATE(FOnQAStopAndReplayClicked);

class SCortexQAToolbar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexQAToolbar) {}
        SLATE_EVENT(FOnQARecordConfirmed, OnRecordConfirmed)
        SLATE_EVENT(FOnQAStopClicked, OnStop)
        SLATE_EVENT(FOnQAStopAndReplayClicked, OnStopAndReplay)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Update the recording state display. */
    void SetRecording(bool bRecording);

    /** Add a step to the live recording ticker. */
    void AddRecordingStep(const FString& StepType, const FString& Target);

    /** Update PIE status text. */
    void SetPIEStatus(const FString& Status);

private:
    void OnRecordButtonClicked();
    void OnNameConfirmed(const FText& Text, ETextCommit::Type CommitType);

    FOnQARecordConfirmed OnRecordConfirmed;
    FOnQAStopClicked OnStop;
    FOnQAStopAndReplayClicked OnStopAndReplay;

    bool bIsRecording = false;
    bool bShowingNameInput = false;
    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<STextBlock> TickerText;
    TSharedPtr<SWidget> ModeSwitcherWidget;
    TSharedPtr<SEditableTextBox> NameInput;
    TSharedPtr<SWidgetSwitcher> ModeSwitcher;

    TArray<FString> RecentSteps; // Last 5 steps for ticker display
};
