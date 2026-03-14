#pragma once

#include "CoreMinimal.h"
#include "Process/CortexStreamEvent.h"
#include "Session/CortexCliSession.h"
#include "Widgets/CortexDisplayTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexChatMessage;
class SCortexChatToolbar;
class SCortexInputArea;
class STableViewBase;

template <typename ItemType>
class SListView;

class SCortexChatPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexChatPanel) {}
        SLATE_ARGUMENT(TWeakPtr<FCortexCliSession>, Session)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SCortexChatPanel();

    friend class FCortexChatPanelConstructTest;
    friend class FCortexChatPanelSessionInitTest;
    friend class FCortexChatPanelFailureCleanupTest;
    friend class FCortexChatPanelCodeBlockTest;
    friend class FCortexChatPanelRejectedSendDoesNotAppendEntriesTest;
    friend class FCortexDisplayRowGroupingTest;

private:
    void SendMessage(const FString& Message);
    void CancelRequest();
    void NewChat();
    void OnStreamEvent(const FCortexStreamEvent& Event);
    void OnTurnComplete(const FCortexTurnResult& Result);
    void OnSessionStateChanged(const FCortexSessionStateChange& Change);

    TSharedRef<ITableRow> GenerateRow(TSharedPtr<FCortexChatDisplayRow> Row, const TSharedRef<STableViewBase>& OwnerTable);
    TArray<TSharedPtr<FCortexChatEntry>> BuildAssistantEntries(const FString& FullText) const;
    void RefreshVisibleEntries();
    void UpdateStateDrivenUi(ECortexSessionState State);

    void ScrollToBottom();

    TSharedPtr<SCortexChatToolbar> ChatToolbar;
    TSharedPtr<SCortexInputArea> InputArea;
    TSharedPtr<SListView<TSharedPtr<FCortexChatDisplayRow>>> ChatList;
    TWeakPtr<FCortexCliSession> SessionWeak;

    TArray<TSharedPtr<FCortexChatDisplayRow>> DisplayRows;
    bool bAutoScroll = true;
    TMap<TSharedPtr<FCortexChatDisplayRow>, TSharedPtr<SCortexChatMessage>> MessageWidgetCache;
};
