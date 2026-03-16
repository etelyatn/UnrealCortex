// Source/CortexFrontend/Private/Widgets/SCortexQASessionList.h
#pragma once

#include "CoreMinimal.h"
#include "QA/CortexQATabTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

DECLARE_DELEGATE_OneParam(FOnQASessionSelected, int32 /* Index */);
DECLARE_DELEGATE_OneParam(FOnQASessionDeleted, int32 /* Index */);

class SCortexQASessionList : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexQASessionList) {}
        SLATE_EVENT(FOnQASessionSelected, OnSessionSelected)
        SLATE_EVENT(FOnQASessionDeleted, OnSessionDeleted)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Update the displayed session list. */
    void SetSessions(const TArray<FCortexQASessionListItem>& InSessions);

    /** Enable/disable interaction (dimmed during recording). */
    void SetEnabled(bool bEnabled);

private:
    TSharedRef<ITableRow> GenerateRow(
        TSharedPtr<FCortexQASessionListItem> Item,
        const TSharedRef<STableViewBase>& OwnerTable);
    void OnSelectionChanged(
        TSharedPtr<FCortexQASessionListItem> Item,
        ESelectInfo::Type SelectInfo);
    TSharedPtr<SWidget> OnContextMenuOpening();

    FOnQASessionSelected OnSessionSelected;
    FOnQASessionDeleted OnSessionDeleted;
    TArray<TSharedPtr<FCortexQASessionListItem>> Items;
    TSharedPtr<SListView<TSharedPtr<FCortexQASessionListItem>>> ListView;
};
