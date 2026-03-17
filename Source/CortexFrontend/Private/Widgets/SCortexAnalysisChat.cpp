// SCortexAnalysisChat.cpp
#include "Widgets/SCortexAnalysisChat.h"

#include "Analysis/CortexFindingTypes.h"
#include "Rendering/CortexChatEntryBuilder.h"
#include "Widgets/SCortexChatMessage.h"
#include "Widgets/SCortexInputArea.h"
#include "Widgets/SCortexProcessingIndicator.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

void SCortexAnalysisChat::Construct(const FArguments& InArgs)
{
	Context = InArgs._Context;
	OnNewFindingDelegate = InArgs._OnNewFinding;

	ChildSlot
	[
		SNew(SVerticalBox)

		// Chat messages list
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(ChatList, SListView<TSharedPtr<FCortexChatDisplayRow>>)
			.ListItemsSource(&DisplayRows)
			.OnGenerateRow(this, &SCortexAnalysisChat::GenerateRow)
			.SelectionMode(ESelectionMode::None)
		]

		// Processing indicator (populated lazily when session is created)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(ProcessingIndicatorBox, SBox)
		]

		// Input area
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(InputArea, SCortexInputArea)
			.OnSendMessage(FOnCortexSendMessage::CreateSP(this, &SCortexAnalysisChat::SendMessage))
			.OnCancel(FOnCortexCancel::CreateSP(this, &SCortexAnalysisChat::CancelRequest))
		]
	];

	// NOTE: Session is null at construction (created when analysis starts).
	// Caller must invoke BindSession() after creating the session.
}

SCortexAnalysisChat::~SCortexAnalysisChat()
{
	if (Context.IsValid() && Context->Session.IsValid())
	{
		Context->Session->OnStreamEvent.RemoveAll(this);
		Context->Session->OnTurnComplete.RemoveAll(this);
		Context->Session->OnStateChanged.RemoveAll(this);
	}
}

void SCortexAnalysisChat::BindSession()
{
	if (Context.IsValid() && Context->Session.IsValid())
	{
		Context->Session->OnStreamEvent.AddSP(this, &SCortexAnalysisChat::OnStreamEvent);
		Context->Session->OnTurnComplete.AddSP(this, &SCortexAnalysisChat::OnTurnComplete);
		Context->Session->OnStateChanged.AddSP(this, &SCortexAnalysisChat::OnSessionStateChanged);

		// Create processing indicator now that session exists
		if (ProcessingIndicatorBox.IsValid())
		{
			ProcessingIndicatorBox->SetContent(
				SNew(SCortexProcessingIndicator)
				.Session(Context->Session)
			);
		}
	}
}

void SCortexAnalysisChat::AddStatusMessage(const FString& Message)
{
	TSharedPtr<FCortexChatEntry> Entry = MakeShared<FCortexChatEntry>();
	Entry->Type = ECortexChatEntryType::AssistantMessage;
	Entry->Text = Message;

	TSharedPtr<FCortexChatDisplayRow> Row = MakeShared<FCortexChatDisplayRow>();
	Row->PrimaryEntry = Entry;
	Row->RowType = ECortexChatRowType::StatusRow;

	StatusRows.Add(Row);
	DisplayRows.Add(Row);

	if (ChatList.IsValid())
	{
		ChatList->RequestListRefresh();
	}
	ScrollToBottom();
}

void SCortexAnalysisChat::SendMessage(const FString& Message)
{
	if (!Context.IsValid() || !Context->Session.IsValid())
	{
		return;
	}

	Context->Session->AddUserPromptEntry(Message);

	FCortexPromptRequest Request;
	Request.Prompt = Message;
	if (!Context->Session->SendPrompt(Request))
	{
		Context->Session->RollbackLastPromptEntries();
	}

	if (InputArea.IsValid())
	{
		InputArea->ClearInput();
	}

	RefreshVisibleEntries();
}

void SCortexAnalysisChat::CancelRequest()
{
	if (Context.IsValid() && Context->Session.IsValid())
	{
		Context->Session->Cancel();
	}
}

void SCortexAnalysisChat::OnStreamEvent(const FCortexStreamEvent& Event)
{
	if (Event.Type == ECortexStreamEventType::SystemError)
	{
		RefreshVisibleEntries();
		if (bAutoScroll)
		{
			ScrollToBottom();
		}
	}
}

void SCortexAnalysisChat::OnTurnComplete(const FCortexTurnResult& Result)
{
	if (!Context.IsValid() || !Context->Session.IsValid())
	{
		return;
	}

	if (!Result.bIsError)
	{
		// Extract findings from the completed assistant response
		const TArray<TSharedPtr<FCortexChatEntry>>& ChatEntries = Context->Session->GetChatEntries();
		if (ChatEntries.Num() > 0)
		{
			const TSharedPtr<FCortexChatEntry>& LastEntry = ChatEntries.Last();
			if (LastEntry.IsValid() && LastEntry->Type == ECortexChatEntryType::AssistantMessage)
			{
				ProcessFindings(LastEntry->Text);
			}
		}
	}

	if (Context->bIsInitialGeneration)
	{
		Context->bIsInitialGeneration = false;
	}

	CollapseStatusMessages(Result);
	RefreshVisibleEntries();
	ScrollToBottom();

	if (InputArea.IsValid())
	{
		InputArea->SetStreaming(false);
	}
}

void SCortexAnalysisChat::ProcessFindings(const FString& FullText)
{
	if (!Context.IsValid())
	{
		return;
	}

	TArray<FCortexAnalysisFinding> NewFindings;
	FCortexChatEntryBuilder::BuildEntries(FullText, &NewFindings);

	for (FCortexAnalysisFinding& Finding : NewFindings)
	{
		// Resolve "node_N" reference from NodeDisplayName placeholder
		if (Finding.NodeDisplayName.StartsWith(TEXT("node_")))
		{
			const FString IdStr = Finding.NodeDisplayName.Mid(5);
			const int32 NodeId = FCString::Atoi(*IdStr);

			FGuid ResolvedGuid;
			if (Context->ResolveNodeId(NodeId, ResolvedGuid))
			{
				Finding.NodeGuid = ResolvedGuid;
				Finding.NodeDisplayName = Context->GetNodeDisplayName(NodeId);
				Finding.GraphName = Context->GetNodeGraphName(NodeId);
			}
		}

		const int32 Idx = Context->AddFinding(Finding);
		OnNewFindingDelegate.ExecuteIfBound(Context->Findings[Idx]);
	}
}

void SCortexAnalysisChat::OnSessionStateChanged(const FCortexSessionStateChange& Change)
{
	if (InputArea.IsValid())
	{
		const bool bActive = Change.NewState == ECortexSessionState::Spawning
			|| Change.NewState == ECortexSessionState::Processing
			|| Change.NewState == ECortexSessionState::Respawning;
		InputArea->SetStreaming(bActive);
		InputArea->SetInputEnabled(
			Change.NewState == ECortexSessionState::Inactive
			|| Change.NewState == ECortexSessionState::Idle);
	}
}

void SCortexAnalysisChat::CollapseStatusMessages(const FCortexTurnResult& Result)
{
	StatusRows.Empty();

	TSharedPtr<FCortexChatEntry> SummaryEntry = MakeShared<FCortexChatEntry>();
	SummaryEntry->Type = ECortexChatEntryType::AssistantMessage;

	if (Result.bIsError)
	{
		SummaryEntry->Text = FString::Printf(TEXT("Analysis failed (%dms): %s"),
			Result.DurationMs, *Result.ResultText);
	}
	else
	{
		SummaryEntry->Text = FString::Printf(TEXT("Analysis completed in %.1fs"),
			Result.DurationMs / 1000.0);
	}

	TSharedPtr<FCortexChatDisplayRow> SummaryRow = MakeShared<FCortexChatDisplayRow>();
	SummaryRow->PrimaryEntry = SummaryEntry;
	SummaryRow->RowType = ECortexChatRowType::StatusRow;
	StatusRows.Add(SummaryRow);
}

void SCortexAnalysisChat::RefreshVisibleEntries()
{
	DisplayRows.Empty();

	// Status messages first (serialization steps, session status, etc.)
	DisplayRows.Append(StatusRows);

	if (Context.IsValid() && Context->Session.IsValid())
	{
		const TArray<TSharedPtr<FCortexChatEntry>>& ChatEntries = Context->Session->GetChatEntries();
		for (const TSharedPtr<FCortexChatEntry>& Entry : ChatEntries)
		{
			// Skip the initial BP JSON user message — it's internal, not user-typed
			if (Entry->Type == ECortexChatEntryType::UserMessage && Entry->TurnIndex <= 1)
			{
				continue;
			}

			TSharedPtr<FCortexChatDisplayRow> Row = MakeShared<FCortexChatDisplayRow>();
			Row->PrimaryEntry = Entry;
			Row->RowType = (Entry->Type == ECortexChatEntryType::UserMessage)
				? ECortexChatRowType::UserMessage
				: ECortexChatRowType::AssistantTurn;
			DisplayRows.Add(Row);
		}
	}

	if (ChatList.IsValid())
	{
		ChatList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SCortexAnalysisChat::GenerateRow(
	TSharedPtr<FCortexChatDisplayRow> Row,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	TSharedRef<SWidget> Content = SNullWidget::NullWidget;

	if (Row.IsValid() && Row->PrimaryEntry.IsValid())
	{
		switch (Row->RowType)
		{
		case ECortexChatRowType::StatusRow:
		{
			Content = SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 6, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("|")))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.3f, 0.5f, 0.8f)))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Row->PrimaryEntry->Text))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.6f, 0.7f)))
					.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
					.AutoWrapText(true)
				];
			break;
		}

		case ECortexChatRowType::UserMessage:
		{
			Content = SNew(SCortexChatMessage)
				.Message(Row->PrimaryEntry->Text)
				.IsUser(true);
			break;
		}

		case ECortexChatRowType::AssistantTurn:
		{
			Content = SNew(SCortexChatMessage)
				.Message(Row->PrimaryEntry->Text)
				.IsUser(false);
			break;
		}

		case ECortexChatRowType::CodeBlock:
		case ECortexChatRowType::ProcessingRow:
		{
			break;
		}
		}
	}

	return SNew(STableRow<TSharedPtr<FCortexChatDisplayRow>>, OwnerTable)
		.Padding(FMargin(8.0f, 4.0f))
		[
			Content
		];
}

void SCortexAnalysisChat::ScrollToBottom()
{
	if (ChatList.IsValid() && DisplayRows.Num() > 0 && bAutoScroll)
	{
		ChatList->RequestScrollIntoView(DisplayRows.Last());
	}
}
