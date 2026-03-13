#pragma once

#include "CoreMinimal.h"
#include "Process/CortexCliRunner.h"
#include "Process/CortexStreamEvent.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexChatMessage;
class SCortexInputArea;
class SCortexToolbar;
class STableViewBase;

template <typename ItemType>
class SListView;

enum class ECortexChatEntryType : uint8
{
    UserMessage,
    AssistantMessage,
    ToolCall,
    CodeBlock
};

struct FCortexChatEntry
{
    ECortexChatEntryType Type = ECortexChatEntryType::AssistantMessage;
    FString Text;
    FString ToolName;
    FString ToolInput;
    FString ToolResult;
    FString ToolCallId;
    int32 DurationMs = 0;
    bool bIsToolComplete = false;
    TSharedPtr<SCortexChatMessage> MessageWidget;
};

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

private:
    void SendMessage(const FString& Message);
    void CancelRequest();
    void NewChat();
    void OnModeChanged(ECortexAccessMode Mode);
    void OnStreamEvent(const FCortexStreamEvent& Event);
    void OnComplete(const FString& FullText, bool bSuccess);

    TSharedRef<ITableRow> GenerateRow(TSharedPtr<FCortexChatEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable);
    TArray<TSharedPtr<FCortexChatEntry>> BuildAssistantEntries(const FString& FullText) const;
    void ReplaceCurrentStreamingEntry(const TArray<TSharedPtr<FCortexChatEntry>>& ReplacementEntries);

    void ScrollToBottom();
    FString GenerateSessionId() const;

    TSharedPtr<SCortexToolbar> Toolbar;
    TSharedPtr<SCortexInputArea> InputArea;
    TSharedPtr<SListView<TSharedPtr<FCortexChatEntry>>> ChatList;

    TArray<TSharedPtr<FCortexChatEntry>> ChatEntries;
    TUniquePtr<FCortexCliRunner> CliRunner;
    FString SessionId;
    bool bAutoScroll = true;
    bool bHasConfirmedSession = false;

    TSharedPtr<FCortexChatEntry> CurrentStreamingEntry;
    FString StreamingText;
};
