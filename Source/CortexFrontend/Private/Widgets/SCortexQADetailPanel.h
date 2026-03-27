// Source/CortexFrontend/Private/Widgets/SCortexQADetailPanel.h
#pragma once

#include "CoreMinimal.h"
#include "QA/CortexQATabTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE(FOnQAReplayClicked);
DECLARE_DELEGATE(FOnQAFastReplayClicked);
DECLARE_DELEGATE(FOnQADeleteClicked);

struct FCortexQADetailStep
{
    int32 Index = 0;
    FString Type;
    FString ParamsText;
    bool bPassed = false;
    bool bHasResult = false;
};

class SCortexQADetailPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexQADetailPanel) {}
        SLATE_EVENT(FOnQAReplayClicked, OnReplay)
        SLATE_EVENT(FOnQAFastReplayClicked, OnFastReplay)
        SLATE_EVENT(FOnQADeleteClicked, OnDelete)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Show details for the given session. Pass null to clear. */
    void SetSession(const FCortexQASessionListItem* Session, const TArray<FCortexQADetailStep>& Steps);

    /** Highlight the current replay step. */
    void SetReplayProgress(int32 CurrentStep);

    /** Clear the panel. */
    void Clear();

private:
    TSharedRef<ITableRow> GenerateStepRow(
        TSharedPtr<FCortexQADetailStep> Item,
        const TSharedRef<STableViewBase>& OwnerTable);

    FOnQAReplayClicked OnReplay;
    FOnQAFastReplayClicked OnFastReplay;
    FOnQADeleteClicked OnDelete;

    TSharedPtr<STextBlock> HeaderText;
    TArray<TSharedPtr<FCortexQADetailStep>> StepItems;
    TSharedPtr<SListView<TSharedPtr<FCortexQADetailStep>>> StepListView;
    int32 ActiveReplayStep = INDEX_NONE;
};
