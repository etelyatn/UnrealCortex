#include "Widgets/SCortexChatPanel.h"

#include "CortexFrontendModule.h"
#include "CortexFrontendSettings.h"
#include "Modules/ModuleManager.h"
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
    FCortexFrontendModule& Module = FModuleManager::LoadModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
    SessionWeak = Module.GetOrCreateSession();

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

    if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
    {
        Session->OnStreamEvent.AddSP(this, &SCortexChatPanel::OnStreamEvent);
        Session->OnTurnComplete.AddSP(this, &SCortexChatPanel::OnTurnComplete);
        Session->OnStateChanged.AddSP(this, &SCortexChatPanel::OnSessionStateChanged);

        RefreshVisibleEntries();
        if (Toolbar.IsValid())
        {
            Toolbar->SetSessionId(Session->GetSessionId());
        }
        UpdateStateDrivenUi(Session->GetState());
    }
}

SCortexChatPanel::~SCortexChatPanel()
{
}

void SCortexChatPanel::SendMessage(const FString& Message)
{
    TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin();
    if (!Session.IsValid())
    {
        return;
    }

    FCortexPromptRequest Request;
    Request.Prompt = Message;
    Request.AccessMode = FCortexFrontendSettings::Get().GetAccessMode();

    Session->AddUserPromptEntry(Message);
    if (!Session->SendPrompt(Request))
    {
        return;
    }

    RefreshVisibleEntries();

    if (InputArea.IsValid())
    {
        InputArea->ClearInput();
    }

    UpdateStateDrivenUi(Session->GetState());
    ScrollToBottom();
}

void SCortexChatPanel::CancelRequest()
{
    if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
    {
        Session->Cancel();
        UpdateStateDrivenUi(Session->GetState());
    }
}

void SCortexChatPanel::NewChat()
{
    if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
    {
        Session->NewChat();
        RefreshVisibleEntries();
        if (Toolbar.IsValid())
        {
            Toolbar->SetSessionId(Session->GetSessionId());
        }
        UpdateStateDrivenUi(Session->GetState());
    }
}

void SCortexChatPanel::OnModeChanged(ECortexAccessMode Mode)
{
    FCortexFrontendSettings::Get().SetAccessMode(Mode);
}

void SCortexChatPanel::OnStreamEvent(const FCortexStreamEvent& Event)
{
    if (Event.Type == ECortexStreamEventType::SessionInit)
    {
        if (Toolbar.IsValid() && !Event.SessionId.IsEmpty())
        {
            Toolbar->SetSessionId(Event.SessionId);
        }
        return;
    }

    RefreshVisibleEntries();
    if (bAutoScroll)
    {
        ScrollToBottom();
    }
}

void SCortexChatPanel::OnTurnComplete(const FCortexTurnResult& Result)
{
    TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin();
    if (!Session.IsValid())
    {
        return;
    }

    if (Result.bIsError)
    {
        TArray<TSharedPtr<FCortexChatEntry>> ErrorEntries;
        TSharedPtr<FCortexChatEntry> ErrorEntry = MakeShared<FCortexChatEntry>();
        ErrorEntry->Type = ECortexChatEntryType::AssistantMessage;
        ErrorEntry->Text = FString::Printf(TEXT("Error: %s"), *Result.ResultText);
        ErrorEntries.Add(ErrorEntry);
        Session->ReplaceStreamingEntry(ErrorEntries);
    }
    else
    {
        Session->ReplaceStreamingEntry(BuildAssistantEntries(Result.ResultText));
    }

    RefreshVisibleEntries();
    UpdateStateDrivenUi(Session->GetState());

    if (InputArea.IsValid())
    {
        InputArea->FocusInput();
    }
    if (bAutoScroll)
    {
        ScrollToBottom();
    }
}

void SCortexChatPanel::OnSessionStateChanged(const FCortexSessionStateChange& Change)
{
    UpdateStateDrivenUi(Change.NewState);
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
        FString Language = TEXT("text");
        int32 FirstNewlineIndex = INDEX_NONE;
        if (BlockText.FindChar(TEXT('\n'), FirstNewlineIndex))
        {
            const FString LangTag = BlockText.Left(FirstNewlineIndex).TrimStartAndEnd();
            if (!LangTag.IsEmpty())
            {
                Language = LangTag;
            }
            BlockText = BlockText.Mid(FirstNewlineIndex + 1);
        }
        BlockText.TrimStartAndEndInline();

        TSharedPtr<FCortexChatEntry> CodeBlockEntry = MakeShared<FCortexChatEntry>();
        CodeBlockEntry->Type = ECortexChatEntryType::CodeBlock;
        CodeBlockEntry->Text = BlockText;
        CodeBlockEntry->Language = Language;
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

void SCortexChatPanel::RefreshVisibleEntries()
{
    MessageWidgetCache.Reset();

    if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
    {
        ChatEntries = Session->GetChatEntries();
    }
    else
    {
        ChatEntries.Reset();
    }

    if (ChatList.IsValid())
    {
        ChatList->RequestListRefresh();
    }
}

void SCortexChatPanel::UpdateStateDrivenUi(ECortexSessionState State)
{
    if (InputArea.IsValid())
    {
        InputArea->SetStreaming(State == ECortexSessionState::Spawning || State == ECortexSessionState::Processing);
        InputArea->SetInputEnabled(State == ECortexSessionState::Inactive || State == ECortexSessionState::Idle);
    }

    if (Toolbar.IsValid())
    {
        Toolbar->SetModeSelectionEnabled(State == ECortexSessionState::Inactive || State == ECortexSessionState::Idle);
    }

    FString StatusText;
    switch (State)
    {
    case ECortexSessionState::Inactive:
        StatusText = TEXT("Ready");
        break;
    case ECortexSessionState::Spawning:
        StatusText = TEXT("Starting...");
        break;
    case ECortexSessionState::Idle:
        StatusText = TEXT("Connected");
        break;
    case ECortexSessionState::Processing:
        StatusText = TEXT("Thinking...");
        break;
    case ECortexSessionState::Cancelling:
        StatusText = TEXT("Cancelling...");
        break;
    case ECortexSessionState::Respawning:
        StatusText = TEXT("Restarting session...");
        break;
    case ECortexSessionState::Terminated:
        StatusText = TEXT("Disconnected");
        break;
    }

    if (Toolbar.IsValid())
    {
        Toolbar->SetStatus(StatusText);
    }
}

TSharedRef<ITableRow> SCortexChatPanel::GenerateRow(TSharedPtr<FCortexChatEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable)
{
    TSharedRef<SWidget> Content = SNullWidget::NullWidget;

    switch (Entry->Type)
    {
    case ECortexChatEntryType::UserMessage:
        Content = SNew(SCortexChatMessage)
            .Message(Entry->Text)
            .IsUser(true);
        break;

    case ECortexChatEntryType::AssistantMessage:
    {
        TSharedPtr<SCortexChatMessage> MessageWidget;
        Content = SAssignNew(MessageWidget, SCortexChatMessage)
            .Message(Entry->Text)
            .IsUser(false);
        MessageWidgetCache.Add(Entry, MessageWidget);
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
            .Language(Entry->Language);
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
