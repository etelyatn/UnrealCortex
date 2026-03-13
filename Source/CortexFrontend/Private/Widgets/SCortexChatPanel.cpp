#include "Widgets/SCortexChatPanel.h"

#include "CortexFrontendModule.h"
#include "CortexFrontendSettings.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Widgets/SCortexChatMessage.h"
#include "Widgets/SCortexCodeBlock.h"
#include "Widgets/SCortexInputArea.h"
#include "Widgets/SCortexToolCallBlock.h"
#include "Widgets/SCortexToolbar.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

void SCortexChatPanel::Construct(const FArguments& InArgs)
{
    SessionId = GenerateSessionId();
    CliRunner = MakeUnique<FCortexCliRunner>();

    const ECortexAccessMode InitialMode = FCortexFrontendSettings::Get().GetAccessMode();

    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SAssignNew(Toolbar, SCortexToolbar)
            .InitialMode(InitialMode)
            .OnModeChanged(this, &SCortexChatPanel::OnModeChanged)
            .OnNewChat(FOnCortexNewChat::CreateSP(this, &SCortexChatPanel::NewChat))
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SSeparator)
        ]
        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        [
            SAssignNew(ChatList, SListView<TSharedPtr<FCortexChatEntry>>)
            .ListItemsSource(&ChatEntries)
            .OnGenerateRow(this, &SCortexChatPanel::GenerateRow)
            .SelectionMode(ESelectionMode::None)
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SSeparator)
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SAssignNew(InputArea, SCortexInputArea)
            .OnSendMessage(FOnCortexSendMessage::CreateSP(this, &SCortexChatPanel::SendMessage))
            .OnCancel(FOnCortexCancel::CreateSP(this, &SCortexChatPanel::CancelRequest))
        ]
    ];

    if (Toolbar.IsValid())
    {
        Toolbar->SetSessionId(SessionId);
    }
}

SCortexChatPanel::~SCortexChatPanel()
{
    if (CliRunner && CliRunner->IsExecuting())
    {
        CliRunner->Cancel();
    }
}

void SCortexChatPanel::SendMessage(const FString& Message)
{
    if (!CliRunner || CliRunner->IsExecuting())
    {
        return;
    }

    TSharedPtr<FCortexChatEntry> UserEntry = MakeShared<FCortexChatEntry>();
    UserEntry->Type = ECortexChatEntryType::UserMessage;
    UserEntry->Text = Message;
    ChatEntries.Add(UserEntry);

    CurrentStreamingEntry = MakeShared<FCortexChatEntry>();
    CurrentStreamingEntry->Type = ECortexChatEntryType::AssistantMessage;
    CurrentStreamingEntry->Text = TEXT("");
    ChatEntries.Add(CurrentStreamingEntry);
    StreamingText.Empty();

    if (ChatList.IsValid())
    {
        ChatList->RequestListRefresh();
    }
    ScrollToBottom();

    if (InputArea.IsValid())
    {
        InputArea->ClearInput();
        InputArea->SetStreaming(true);
    }
    if (Toolbar.IsValid())
    {
        Toolbar->SetStatus(TEXT("Connecting..."));
    }

    FCortexChatRequest Request;
    Request.Prompt = Message;
    Request.SessionId = SessionId;
    Request.bIsFirstMessage = !bHasConfirmedSession;
    Request.AccessMode = FCortexFrontendSettings::Get().GetAccessMode();
    Request.bSkipPermissions = FCortexFrontendSettings::Get().GetSkipPermissions();
    Request.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

    FString McpPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));
    if (FPaths::FileExists(McpPath))
    {
        Request.McpConfigPath = FPaths::ConvertRelativePathToFull(McpPath);
    }

    CliRunner->ExecuteAsync(
        Request,
        FOnCortexComplete::CreateSP(this, &SCortexChatPanel::OnComplete),
        FOnCortexStreamEvent::CreateSP(this, &SCortexChatPanel::OnStreamEvent));
}

void SCortexChatPanel::CancelRequest()
{
    if (CliRunner)
    {
        CliRunner->Cancel();
    }
}

void SCortexChatPanel::NewChat()
{
    if (CliRunner && CliRunner->IsExecuting())
    {
        CliRunner->Cancel();
    }

    ChatEntries.Empty();
    if (ChatList.IsValid())
    {
        ChatList->RequestListRefresh();
    }

    SessionId = GenerateSessionId();
    CurrentStreamingEntry.Reset();
    StreamingText.Empty();
    bAutoScroll = true;
    bHasConfirmedSession = false;

    if (Toolbar.IsValid())
    {
        Toolbar->SetSessionId(SessionId);
        Toolbar->SetStatus(FString());
    }
    if (InputArea.IsValid())
    {
        InputArea->SetStreaming(false);
        InputArea->FocusInput();
    }
}

void SCortexChatPanel::OnModeChanged(ECortexAccessMode Mode)
{
    FCortexFrontendSettings::Get().SetAccessMode(Mode);
}

void SCortexChatPanel::OnStreamEvent(const FCortexStreamEvent& Event)
{
    switch (Event.Type)
    {
    case ECortexStreamEventType::SessionInit:
        if (!Event.SessionId.IsEmpty())
        {
            SessionId = Event.SessionId;
            bHasConfirmedSession = true;
            if (Toolbar.IsValid())
            {
                Toolbar->SetSessionId(SessionId);
            }
        }
        if (Toolbar.IsValid())
        {
            Toolbar->SetStatus(TEXT("Connected"));
        }
        break;

    case ECortexStreamEventType::TextContent:
        StreamingText += Event.Text;
        if (CurrentStreamingEntry.IsValid())
        {
            CurrentStreamingEntry->Text = StreamingText;
            if (CurrentStreamingEntry->MessageWidget.IsValid())
            {
                CurrentStreamingEntry->MessageWidget->SetText(StreamingText);
            }
        }
        if (Toolbar.IsValid())
        {
            Toolbar->SetStatus(TEXT("Streaming..."));
        }
        if (bAutoScroll)
        {
            ScrollToBottom();
        }
        break;

    case ECortexStreamEventType::ToolUse:
    {
        TSharedPtr<FCortexChatEntry> ToolEntry = MakeShared<FCortexChatEntry>();
        ToolEntry->Type = ECortexChatEntryType::ToolCall;
        ToolEntry->ToolName = Event.ToolName;
        ToolEntry->ToolCallId = Event.ToolCallId;
        ToolEntry->ToolInput = Event.ToolInput;
        ToolEntry->bIsToolComplete = false;
        ChatEntries.Add(ToolEntry);
        if (ChatList.IsValid())
        {
            ChatList->RequestListRefresh();
        }
        if (bAutoScroll)
        {
            ScrollToBottom();
        }
        break;
    }

    case ECortexStreamEventType::ToolResult:
        for (int32 Index = ChatEntries.Num() - 1; Index >= 0; --Index)
        {
            if (ChatEntries[Index]->Type == ECortexChatEntryType::ToolCall && ChatEntries[Index]->ToolCallId == Event.ToolCallId)
            {
                ChatEntries[Index]->ToolResult = Event.ToolResultContent;
                ChatEntries[Index]->bIsToolComplete = true;
                if (ChatList.IsValid())
                {
                    ChatList->RequestListRefresh();
                }
                break;
            }
        }
        break;

    case ECortexStreamEventType::Result:
        if (Toolbar.IsValid())
        {
            Toolbar->SetStatus(FString::Printf(TEXT("Done %dms"), Event.DurationMs));
        }
        break;

    default:
        break;
    }
}

void SCortexChatPanel::OnComplete(const FString& FullText, bool bSuccess)
{
    const bool bHadEmptyStreamingEntry = CurrentStreamingEntry.IsValid() && CurrentStreamingEntry->Text.IsEmpty();

    if (InputArea.IsValid())
    {
        InputArea->SetStreaming(false);
    }

    if (bSuccess)
    {
        const FString FinalAssistantText = !FullText.IsEmpty() ? FullText : StreamingText;
        ReplaceCurrentStreamingEntry(BuildAssistantEntries(FinalAssistantText));
    }
    else
    {
        if (bHadEmptyStreamingEntry && CurrentStreamingEntry.IsValid())
        {
            ChatEntries.RemoveSingle(CurrentStreamingEntry);
        }
        if (!FullText.IsEmpty())
        {
            TSharedPtr<FCortexChatEntry> ErrorEntry = MakeShared<FCortexChatEntry>();
            ErrorEntry->Type = ECortexChatEntryType::AssistantMessage;
            ErrorEntry->Text = FString::Printf(TEXT("Error: %s"), *FullText);
            ChatEntries.Add(ErrorEntry);
        }
        if (ChatList.IsValid())
        {
            ChatList->RequestListRefresh();
        }
        if (Toolbar.IsValid())
        {
            Toolbar->SetStatus(TEXT("Error"));
        }
    }

    CurrentStreamingEntry.Reset();
    StreamingText.Empty();

    if (InputArea.IsValid())
    {
        InputArea->FocusInput();
    }
    if (bAutoScroll)
    {
        ScrollToBottom();
    }
}

TArray<TSharedPtr<FCortexChatEntry>> SCortexChatPanel::BuildAssistantEntries(const FString& FullText) const
{
    TArray<TSharedPtr<FCortexChatEntry>> Entries;
    FString RemainingText = FullText;

    while (!RemainingText.IsEmpty())
    {
        const int32 BlockStart = RemainingText.Find(TEXT("```"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
        if (BlockStart == INDEX_NONE)
        {
            TSharedPtr<FCortexChatEntry> TextEntry = MakeShared<FCortexChatEntry>();
            TextEntry->Type = ECortexChatEntryType::AssistantMessage;
            TextEntry->Text = RemainingText;
            Entries.Add(TextEntry);
            break;
        }

        const FString BeforeBlock = RemainingText.Left(BlockStart);
        if (!BeforeBlock.IsEmpty())
        {
            TSharedPtr<FCortexChatEntry> TextEntry = MakeShared<FCortexChatEntry>();
            TextEntry->Type = ECortexChatEntryType::AssistantMessage;
            TextEntry->Text = BeforeBlock;
            Entries.Add(TextEntry);
        }

        const int32 BlockContentStart = BlockStart + 3;
        const int32 BlockEnd = RemainingText.Find(TEXT("```"), ESearchCase::CaseSensitive, ESearchDir::FromStart, BlockContentStart);
        if (BlockEnd == INDEX_NONE)
        {
            TSharedPtr<FCortexChatEntry> TextEntry = MakeShared<FCortexChatEntry>();
            TextEntry->Type = ECortexChatEntryType::AssistantMessage;
            TextEntry->Text = RemainingText.Mid(BlockStart);
            Entries.Add(TextEntry);
            break;
        }

        FString BlockText = RemainingText.Mid(BlockContentStart, BlockEnd - BlockContentStart);
        int32 FirstNewlineIndex = INDEX_NONE;
        if (BlockText.FindChar(TEXT('\n'), FirstNewlineIndex))
        {
            BlockText = BlockText.Mid(FirstNewlineIndex + 1);
        }
        BlockText.TrimStartAndEndInline();

        TSharedPtr<FCortexChatEntry> CodeBlockEntry = MakeShared<FCortexChatEntry>();
        CodeBlockEntry->Type = ECortexChatEntryType::CodeBlock;
        CodeBlockEntry->Text = BlockText;
        Entries.Add(CodeBlockEntry);

        RemainingText = RemainingText.Mid(BlockEnd + 3);
    }

    if (Entries.Num() == 0)
    {
        TSharedPtr<FCortexChatEntry> TextEntry = MakeShared<FCortexChatEntry>();
        TextEntry->Type = ECortexChatEntryType::AssistantMessage;
        TextEntry->Text = FullText;
        Entries.Add(TextEntry);
    }

    return Entries;
}

void SCortexChatPanel::ReplaceCurrentStreamingEntry(const TArray<TSharedPtr<FCortexChatEntry>>& ReplacementEntries)
{
    if (!CurrentStreamingEntry.IsValid())
    {
        return;
    }

    const int32 CurrentIndex = ChatEntries.IndexOfByKey(CurrentStreamingEntry);
    if (CurrentIndex == INDEX_NONE)
    {
        return;
    }

    ChatEntries.RemoveAt(CurrentIndex);
    ChatEntries.Insert(ReplacementEntries, CurrentIndex);

    if (ChatList.IsValid())
    {
        ChatList->RequestListRefresh();
    }
}

TSharedRef<ITableRow> SCortexChatPanel::GenerateRow(TSharedPtr<FCortexChatEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable)
{
    TSharedRef<SWidget> Content = SNullWidget::NullWidget;

    switch (Entry->Type)
    {
    case ECortexChatEntryType::UserMessage:
    {
        TSharedPtr<SCortexChatMessage> MessageWidget;
        Content = SAssignNew(MessageWidget, SCortexChatMessage)
            .Message(Entry->Text)
            .IsUser(true);
        break;
    }

    case ECortexChatEntryType::AssistantMessage:
    {
        TSharedPtr<SCortexChatMessage> MessageWidget;
        Content = SAssignNew(MessageWidget, SCortexChatMessage)
            .Message(Entry->Text)
            .IsUser(false);
        Entry->MessageWidget = MessageWidget;
        break;
    }

    case ECortexChatEntryType::ToolCall:
        Content = SNew(SCortexToolCallBlock)
            .ToolName(Entry->ToolName)
            .ToolInput(Entry->ToolInput)
            .ToolResult(Entry->ToolResult)
            .DurationMs(Entry->DurationMs)
            .bIsComplete(Entry->bIsToolComplete);
        break;

    case ECortexChatEntryType::CodeBlock:
        Content = SNew(SCortexCodeBlock)
            .Code(Entry->Text)
            .Language(TEXT("text"));
        break;
    }

    return SNew(STableRow<TSharedPtr<FCortexChatEntry>>, OwnerTable)
        .Padding(FMargin(8.0f, 4.0f))
        [
            Content
        ];
}

void SCortexChatPanel::ScrollToBottom()
{
    if (ChatList.IsValid() && ChatEntries.Num() > 0)
    {
        ChatList->RequestScrollIntoView(ChatEntries.Last());
    }
}

FString SCortexChatPanel::GenerateSessionId() const
{
    return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
}
