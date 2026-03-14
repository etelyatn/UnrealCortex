#pragma once

#include "CoreMinimal.h"
#include "Process/CortexStreamEvent.h"
#include "Session/CortexCliSession.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexChatMessage;
class SCortexInputArea;
class SCortexToolbar;
class STableViewBase;

template <typename ItemType>
class SListView;

class SCortexChatPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexChatPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SCortexChatPanel();

    friend class FCortexChatPanelConstructTest;
    friend class FCortexChatPanelSessionInitTest;
    friend class FCortexChatPanelFailureCleanupTest;
    friend class FCortexChatPanelCodeBlockTest;
    friend class FCortexChatPanelRejectedSendDoesNotAppendEntriesTest;

private:
    void SendMessage(const FString& Message);
    void CancelRequest();
    void NewChat();
    void OnModeChanged(ECortexAccessMode Mode);
    void OnStreamEvent(const FCortexStreamEvent& Event);
    void OnTurnComplete(const FCortexTurnResult& Result);
    void OnSessionStateChanged(const FCortexSessionStateChange& Change);

    TSharedRef<ITableRow> GenerateRow(TSharedPtr<FCortexChatEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable);
    TArray<TSharedPtr<FCortexChatEntry>> BuildAssistantEntries(const FString& FullText) const;
    void RefreshVisibleEntries();
    void UpdateStateDrivenUi(ECortexSessionState State);

    void ScrollToBottom();

    TSharedPtr<SCortexToolbar> Toolbar;
    TSharedPtr<SCortexInputArea> InputArea;
    TSharedPtr<SListView<TSharedPtr<FCortexChatEntry>>> ChatList;
    TWeakPtr<FCortexCliSession> SessionWeak;

    TArray<TSharedPtr<FCortexChatEntry>> ChatEntries;
    bool bAutoScroll = true;
    TMap<TSharedPtr<FCortexChatEntry>, TSharedPtr<SCortexChatMessage>> MessageWidgetCache;
};
