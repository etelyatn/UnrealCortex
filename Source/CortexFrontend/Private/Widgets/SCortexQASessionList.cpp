// Source/CortexFrontend/Private/Widgets/SCortexQASessionList.cpp
#include "Widgets/SCortexQASessionList.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/MessageDialog.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

void SCortexQASessionList::Construct(const FArguments& InArgs)
{
    OnSessionSelected = InArgs._OnSessionSelected;
    OnSessionDeleted = InArgs._OnSessionDeleted;

    ChildSlot
    [
        SAssignNew(ListView, SListView<TSharedPtr<FCortexQASessionListItem>>)
        .ListItemsSource(&Items)
        .OnGenerateRow(this, &SCortexQASessionList::GenerateRow)
        .OnSelectionChanged(this, &SCortexQASessionList::OnSelectionChanged)
        .OnContextMenuOpening(this, &SCortexQASessionList::OnContextMenuOpening)
        .SelectionMode(ESelectionMode::Single)
    ];
}

void SCortexQASessionList::SetSessions(const TArray<FCortexQASessionListItem>& InSessions)
{
    Items.Empty();
    for (const FCortexQASessionListItem& Session : InSessions)
    {
        Items.Add(MakeShared<FCortexQASessionListItem>(Session));
    }
    if (ListView.IsValid())
    {
        ListView->RequestListRefresh();
    }
}

void SCortexQASessionList::SetEnabled(bool bEnabled)
{
    if (ListView.IsValid())
    {
        ListView->SetEnabled(bEnabled);
    }
}

TSharedRef<ITableRow> SCortexQASessionList::GenerateRow(
    TSharedPtr<FCortexQASessionListItem> Item,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    // Status icon: green check, red X, gray circle, or blue AI badge
    FString StatusIcon;
    if (!Item->bHasBeenRun)
    {
        StatusIcon = TEXT("o"); // gray circle placeholder
    }
    else if (Item->bLastRunPassed)
    {
        StatusIcon = TEXT("+"); // green check placeholder
    }
    else
    {
        StatusIcon = TEXT("x"); // red X placeholder
    }

    FString SourceBadge;
    if (Item->Source == TEXT("ai_generated"))
    {
        SourceBadge = TEXT(" [AI]");
    }

    return SNew(STableRow<TSharedPtr<FCortexQASessionListItem>>, OwnerTable)
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.f, 2.f)
        [
            SNew(STextBlock)
            .Text(FText::FromString(StatusIcon))
        ]
        + SHorizontalBox::Slot()
        .FillWidth(1.f)
        .Padding(4.f, 2.f)
        [
            SNew(STextBlock)
            .Text(FText::FromString(Item->Name + SourceBadge))
        ]
    ];
}

void SCortexQASessionList::OnSelectionChanged(
    TSharedPtr<FCortexQASessionListItem> Item,
    ESelectInfo::Type SelectInfo)
{
    if (!Item.IsValid())
    {
        OnSessionSelected.ExecuteIfBound(INDEX_NONE);
        return;
    }

    const int32 Index = Items.IndexOfByPredicate([&Item](const TSharedPtr<FCortexQASessionListItem>& Other)
    {
        return Other.Get() == Item.Get();
    });

    OnSessionSelected.ExecuteIfBound(Index);
}

TSharedPtr<SWidget> SCortexQASessionList::OnContextMenuOpening()
{
    TArray<TSharedPtr<FCortexQASessionListItem>> Selected = ListView->GetSelectedItems();
    if (Selected.Num() == 0)
    {
        return nullptr;
    }

    const int32 Index = Items.IndexOfByPredicate([&Selected](const TSharedPtr<FCortexQASessionListItem>& Other)
    {
        return Other.Get() == Selected[0].Get();
    });

    FMenuBuilder MenuBuilder(true, nullptr);

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Rename")),
        FText::GetEmpty(),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([this, Index]()
        {
            // TODO: Show inline rename editor. Phase 1: no-op placeholder.
        })));

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Delete")),
        FText::GetEmpty(),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([this, Index]()
        {
            const EAppReturnType::Type Result = FMessageDialog::Open(
                EAppMsgType::YesNo,
                FText::FromString(TEXT("Delete this session?")));
            if (Result == EAppReturnType::Yes)
            {
                OnSessionDeleted.ExecuteIfBound(Index);
            }
        })));

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Duplicate")),
        FText::GetEmpty(),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([this, Index]()
        {
            // TODO: Duplicate session file. Phase 1: no-op placeholder.
        })));

    return MenuBuilder.MakeWidget();
}
