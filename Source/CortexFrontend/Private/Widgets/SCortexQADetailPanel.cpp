// Source/CortexFrontend/Private/Widgets/SCortexQADetailPanel.cpp
#include "Widgets/SCortexQADetailPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Layout/SScrollBox.h"

void SCortexQADetailPanel::Construct(const FArguments& InArgs)
{
    OnReplay = InArgs._OnReplay;
    OnFastReplay = InArgs._OnFastReplay;
    OnDelete = InArgs._OnDelete;

    ChildSlot
    [
        SNew(SVerticalBox)

        // Header
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(8.f, 4.f)
        [
            SAssignNew(HeaderText, STextBlock)
            .Text(FText::FromString(TEXT("Select a session")))
        ]

        // Action buttons
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(8.f, 2.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.f, 0.f, 4.f, 0.f)
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Replay")))
                .ToolTipText(FText::FromString(TEXT("Smooth replay — walks between recorded positions")))
                .OnClicked_Lambda([this]()
                {
                    OnReplay.ExecuteIfBound();
                    return FReply::Handled();
                })
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.f, 0.f, 4.f, 0.f)
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Fast Replay")))
                .ToolTipText(FText::FromString(TEXT("Teleport replay — jumps between positions with timing")))
                .OnClicked_Lambda([this]()
                {
                    OnFastReplay.ExecuteIfBound();
                    return FReply::Handled();
                })
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Delete")))
                .OnClicked_Lambda([this]()
                {
                    OnDelete.ExecuteIfBound();
                    return FReply::Handled();
                })
            ]
        ]

        // Step list
        + SVerticalBox::Slot()
        .FillHeight(1.f)
        .Padding(8.f, 4.f)
        [
            SAssignNew(StepListView, SListView<TSharedPtr<FCortexQADetailStep>>)
            .ListItemsSource(&StepItems)
            .OnGenerateRow(this, &SCortexQADetailPanel::GenerateStepRow)
            .SelectionMode(ESelectionMode::None)
        ]
    ];
}

void SCortexQADetailPanel::SetSession(
    const FCortexQASessionListItem* Session,
    const TArray<FCortexQADetailStep>& Steps)
{
    if (Session == nullptr)
    {
        Clear();
        return;
    }

    // Update header: name, map, date, step count, last result
    FString HeaderStr = FString::Printf(
        TEXT("%s  |  %s  |  %s  |  %d steps"),
        *Session->Name,
        *Session->MapPath,
        *Session->RecordedAt.ToString(TEXT("%Y-%m-%d %H:%M")),
        Session->StepCount);

    if (Session->bHasBeenRun)
    {
        HeaderStr += Session->bLastRunPassed ? TEXT("  |  PASSED") : TEXT("  |  FAILED");
    }

    if (HeaderText.IsValid())
    {
        HeaderText->SetText(FText::FromString(HeaderStr));
    }

    // Update step list
    StepItems.Empty();
    for (const FCortexQADetailStep& Step : Steps)
    {
        StepItems.Add(MakeShared<FCortexQADetailStep>(Step));
    }

    if (StepListView.IsValid())
    {
        StepListView->RequestListRefresh();
    }
}

void SCortexQADetailPanel::SetReplayProgress(int32 CurrentStep)
{
    ActiveReplayStep = CurrentStep;
    if (StepListView.IsValid())
    {
        StepListView->RequestListRefresh();
    }
}

void SCortexQADetailPanel::Clear()
{
    if (HeaderText.IsValid())
    {
        HeaderText->SetText(FText::FromString(TEXT("Select a session")));
    }
    StepItems.Empty();
    if (StepListView.IsValid())
    {
        StepListView->RequestListRefresh();
    }
}

TSharedRef<ITableRow> SCortexQADetailPanel::GenerateStepRow(
    TSharedPtr<FCortexQADetailStep> Item,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    FString StatusStr;
    if (Item->bHasResult)
    {
        StatusStr = Item->bPassed ? TEXT("+") : TEXT("x");
    }
    else if (Item->Index == ActiveReplayStep)
    {
        StatusStr = TEXT(">");
    }

    return SNew(STableRow<TSharedPtr<FCortexQADetailStep>>, OwnerTable)
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.f, 1.f)
        [
            SNew(STextBlock)
            .Text(FText::FromString(FString::Printf(TEXT("%d"), Item->Index)))
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(8.f, 1.f)
        [
            SNew(STextBlock)
            .Text(FText::FromString(Item->Type))
        ]
        + SHorizontalBox::Slot()
        .FillWidth(1.f)
        .Padding(8.f, 1.f)
        [
            SNew(STextBlock)
            .Text(FText::FromString(Item->ParamsText))
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.f, 1.f)
        [
            SNew(STextBlock)
            .Text(FText::FromString(StatusStr))
        ]
    ];
}
