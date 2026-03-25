#include "Widgets/SCortexChatPanel.h"

#include "CortexFrontendModule.h"
#include "CortexFrontendSettings.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SCortexChatMessage.h"
#include "Widgets/SCortexCodeBlock.h"
#include "Widgets/SCortexInputArea.h"
#include "Widgets/SCortexProcessingIndicator.h"
#include "Widgets/SCortexTableBlock.h"
#include "Widgets/SCortexToolCallBlock.h"
#include "Widgets/SCortexChatToolbar.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

void SCortexChatPanel::Construct(const FArguments& InArgs)
{
    // Use injected session if provided, otherwise fall back to module
    if (InArgs._Session.IsValid())
    {
        SessionWeak = InArgs._Session;
    }
    else
    {
        FCortexFrontendModule& Module = FModuleManager::LoadModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
        SessionWeak = Module.GetOrCreateSession();
    }

    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SAssignNew(ChatToolbar, SCortexChatToolbar)
            .OnNewChat(FOnCortexNewChat::CreateSP(this, &SCortexChatPanel::NewChat))
            .Session(SessionWeak)
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SSeparator)
        ]
        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        [
            SAssignNew(ChatList, SListView<TSharedPtr<FCortexChatDisplayRow>>)
            .ListItemsSource(&DisplayRows)
            .OnGenerateRow(this, &SCortexChatPanel::GenerateRow)
            .SelectionMode(ESelectionMode::None)
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SAssignNew(ProcessingIndicator, SCortexProcessingIndicator)
            .Session(SessionWeak)
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

    // Create stable greeting row (reused across refreshes for cache stability)
    {
        TSharedPtr<FCortexChatEntry> GreetingEntry = MakeShared<FCortexChatEntry>();
        GreetingEntry->Type = ECortexChatEntryType::AssistantMessage;
        GreetingEntry->Text = TEXT("Send a message to start a new session.");

        GreetingRow = MakeShared<FCortexChatDisplayRow>();
        GreetingRow->RowType = ECortexChatRowType::AssistantTurn;
        GreetingRow->PrimaryEntry = GreetingEntry;
    }

    if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
    {
        Session->OnStreamEvent.AddSP(this, &SCortexChatPanel::OnStreamEvent);
        Session->OnTurnComplete.AddSP(this, &SCortexChatPanel::OnTurnComplete);
        Session->OnStateChanged.AddSP(this, &SCortexChatPanel::OnSessionStateChanged);

        if (ChatToolbar.IsValid())
        {
            ChatToolbar->SetSessionId(Session->GetSessionId());
        }
        UpdateStateDrivenUi(Session->GetState());
    }

    // Initial build
    RebuildStableRows();
    RefreshVisibleEntries();
}

SCortexChatPanel::~SCortexChatPanel()
{
    if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
    {
        Session->OnStreamEvent.RemoveAll(this);
        Session->OnTurnComplete.RemoveAll(this);
        Session->OnStateChanged.RemoveAll(this);
    }
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
        Session->RollbackLastPromptEntries();
        RebuildStableRows();
        RefreshVisibleEntries();
        return;
    }

    RebuildStableRows();
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
        Session->NewChat();  // Calls CleanupProcess() internally — kills old CLI subprocess
        Session->Connect();
        if (ChatToolbar.IsValid())
        {
            ChatToolbar->SetSessionId(Session->GetSessionId());
        }
        RebuildStableRows();
        RefreshVisibleEntries();
    }
}

void SCortexChatPanel::OnStreamEvent(const FCortexStreamEvent& Event)
{
    if (Event.Type == ECortexStreamEventType::SessionInit)
    {
        if (ChatToolbar.IsValid() && !Event.SessionId.IsEmpty())
        {
            ChatToolbar->SetSessionId(Event.SessionId);
        }
        return;
    }

    // Text streaming (ContentBlockDelta) is NOT shown in the chat list.
    // The full formatted response appears only on turn complete.
    // The processing indicator widget handles real-time status display.
    if (Event.Type == ECortexStreamEventType::ContentBlockDelta)
    {
        return;
    }

    // SystemError events show immediately
    if (Event.Type == ECortexStreamEventType::SystemError)
    {
        RefreshVisibleEntries();
        if (bAutoScroll)
        {
            ScrollToBottom();
        }
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

    RebuildStableRows();
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
    RefreshVisibleEntries();  // Inject greeting row on Idle, remove on other states
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

void SCortexChatPanel::RebuildStableRows()
{
    StableRows.Reset();

    TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin();
    if (!Session.IsValid())
    {
        return;
    }

    const TArray<TSharedPtr<FCortexChatEntry>>& Entries = Session->GetChatEntries();
    const TSharedPtr<FCortexChatEntry> StreamingEntry = Session->GetCurrentStreamingEntry();

    // Pass 1: collect tool calls per turn
    TMap<int32, TArray<TSharedPtr<FCortexChatEntry>>> ToolCallsByTurn;
    for (const TSharedPtr<FCortexChatEntry>& E : Entries)
    {
        if (E->Type == ECortexChatEntryType::ToolCall)
        {
            ToolCallsByTurn.FindOrAdd(E->TurnIndex).Add(E);
        }
    }

    // Pass 2: build stable display rows (skip the live streaming entry)
    TSet<int32> ConsumedToolTurns;
    for (const TSharedPtr<FCortexChatEntry>& E : Entries)
    {
        if (E == StreamingEntry)
        {
            continue;  // Handled fresh in RefreshVisibleEntries
        }

        switch (E->Type)
        {
        case ECortexChatEntryType::UserMessage:
        {
            TSharedPtr<FCortexChatDisplayRow> Row = MakeShared<FCortexChatDisplayRow>();
            Row->RowType = ECortexChatRowType::UserMessage;
            Row->PrimaryEntry = E;
            StableRows.Add(Row);
            break;
        }

        case ECortexChatEntryType::AssistantMessage:
        {
            TSharedPtr<FCortexChatDisplayRow> Row = MakeShared<FCortexChatDisplayRow>();
            Row->RowType = ECortexChatRowType::AssistantTurn;
            Row->PrimaryEntry = E;
            if (!ConsumedToolTurns.Contains(E->TurnIndex))
            {
                if (TArray<TSharedPtr<FCortexChatEntry>>* ToolCalls = ToolCallsByTurn.Find(E->TurnIndex))
                {
                    Row->ToolCalls = *ToolCalls;
                    ConsumedToolTurns.Add(E->TurnIndex);
                }
            }
            StableRows.Add(Row);
            break;
        }

        case ECortexChatEntryType::CodeBlock:
        {
            TSharedPtr<FCortexChatDisplayRow> Row = MakeShared<FCortexChatDisplayRow>();
            Row->RowType = ECortexChatRowType::CodeBlock;
            Row->PrimaryEntry = E;
            StableRows.Add(Row);
            break;
        }

        case ECortexChatEntryType::ToolCall:
            break;  // Consumed via ToolCallsByTurn

        case ECortexChatEntryType::Table:
        {
            TSharedPtr<FCortexChatDisplayRow> TableRow = MakeShared<FCortexChatDisplayRow>();
            TableRow->RowType = ECortexChatRowType::TableBlock;
            TableRow->PrimaryEntry = E;
            StableRows.Add(TableRow);
            break;
        }
        }
    }
}

void SCortexChatPanel::RefreshVisibleEntries()
{
    TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin();

    // Compose DisplayRows from stable base + optional streaming row + optional processing tail
    DisplayRows = StableRows;

    if (Session.IsValid())
    {
        // Re-add greeting at front when idle/inactive with no messages
        const bool bShowGreeting = (Session->GetState() == ECortexSessionState::Idle
            || Session->GetState() == ECortexSessionState::Inactive)
            && Session->GetChatEntries().IsEmpty();
        if (bShowGreeting && GreetingRow.IsValid())
        {
            DisplayRows.Insert(GreetingRow, 0);
        }

        // Streaming text is NOT shown during processing.
        // The full formatted response appears on turn complete.
        // Processing status is shown by the SCortexProcessingIndicator widget.
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
        const bool bActive = State == ECortexSessionState::Spawning
            || State == ECortexSessionState::Processing
            || State == ECortexSessionState::Respawning;
        InputArea->SetStreaming(bActive);
        InputArea->SetInputEnabled(State == ECortexSessionState::Inactive || State == ECortexSessionState::Idle);
    }
}

TSharedRef<ITableRow> SCortexChatPanel::GenerateRow(TSharedPtr<FCortexChatDisplayRow> Row, const TSharedRef<STableViewBase>& OwnerTable)
{
    TSharedRef<SWidget> Content = SNullWidget::NullWidget;

    // Greeting row — centered muted text, no role label
    if (Row == GreetingRow)
    {
        Content = SNew(SBox)
            .HAlign(HAlign_Center)
            .Padding(0.0f, 40.0f, 0.0f, 8.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Row->PrimaryEntry->Text))
                .Font(FCoreStyle::GetDefaultFontStyle("Italic", 10))
                .ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
            ];
    }
    else
    {
        switch (Row->RowType)
        {
        case ECortexChatRowType::UserMessage:
            Content = SNew(SCortexChatMessage)
                .Message(Row->PrimaryEntry->Text)
                .IsUser(true);
            break;

        case ECortexChatRowType::AssistantTurn:
        {
            TSharedRef<SVerticalBox> TurnBox = SNew(SVerticalBox);

            // Assistant text (includes "Claude" role label)
            TurnBox->AddSlot()
            .AutoHeight()
            [
                SNew(SCortexChatMessage)
                .Message(Row->PrimaryEntry->Text)
                .IsUser(false)
            ];

            // Tool calls below the role label
            if (Row->ToolCalls.Num() > 0)
            {
                TWeakPtr<SListView<TSharedPtr<FCortexChatDisplayRow>>> WeakChatList = ChatList;
                TurnBox->AddSlot()
                .AutoHeight()
                .Padding(8.0f, 0.0f, 8.0f, 4.0f)
                [
                    SNew(SCortexToolCallBlock)
                    .ToolCalls(Row->ToolCalls)
                    .OnToggled(FSimpleDelegate::CreateLambda([WeakChatList]()
                    {
                        if (TSharedPtr<SListView<TSharedPtr<FCortexChatDisplayRow>>> List = WeakChatList.Pin())
                        {
                            List->RequestListRefresh();
                        }
                    }))
                ];
            }

            Content = TurnBox;
            break;
        }

        case ECortexChatRowType::CodeBlock:
            Content = SNew(SCortexCodeBlock)
                .Code(Row->PrimaryEntry->Text)
                .Language(Row->PrimaryEntry->Language);
            break;

        case ECortexChatRowType::ProcessingRow:
            break;  // Handled by SCortexProcessingIndicator widget

        case ECortexChatRowType::TableBlock:
            Content = SNew(SCortexTableBlock)
                .Headers(Row->PrimaryEntry->TableHeaders)
                .Rows(Row->PrimaryEntry->TableRows);
            break;

        default:
            break;
        }
    }

    return SNew(STableRow<TSharedPtr<FCortexChatDisplayRow>>, OwnerTable)
        .Padding(FMargin(8.0f, 4.0f))
        [
            Content
        ];
}

void SCortexChatPanel::ScrollToBottom()
{
    if (ChatList.IsValid() && DisplayRows.Num() > 0)
    {
        ChatList->RequestScrollIntoView(DisplayRows.Last());
    }
}
