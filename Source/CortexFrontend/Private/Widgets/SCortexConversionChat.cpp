#include "Widgets/SCortexConversionChat.h"

#include "CortexFrontendModule.h"
#include "Rendering/CortexMarkdownParser.h"
#include "Session/CortexCliSession.h"
#include "Widgets/SCortexChatMessage.h"
#include "Widgets/SCortexCodeBlock.h"
#include "Widgets/SCortexInputArea.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

void SCortexConversionChat::Construct(const FArguments& InArgs)
{
	Context = InArgs._Context;

	ChildSlot
	[
		SNew(SVerticalBox)

		// Chat message list
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(ChatList, SListView<TSharedPtr<FCortexChatDisplayRow>>)
			.ListItemsSource(&DisplayRows)
			.OnGenerateRow(this, &SCortexConversionChat::GenerateRow)
			.SelectionMode(ESelectionMode::None)
		]

		// Input area
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(InputArea, SCortexInputArea)
			.OnSendMessage(FOnCortexSendMessage::CreateSP(this, &SCortexConversionChat::SendMessage))
			.OnCancel(FOnCortexCancel::CreateSP(this, &SCortexConversionChat::CancelRequest))
		]
	];

	// NOTE: Session is null at construction (created when user clicks Convert).
	// SCortexConversionTab calls BindSession() after creating the session.
}

void SCortexConversionChat::BindSession()
{
	if (Context.IsValid() && Context->Session.IsValid())
	{
		Context->Session->OnStreamEvent.AddSP(this, &SCortexConversionChat::OnStreamEvent);
		Context->Session->OnTurnComplete.AddSP(this, &SCortexConversionChat::OnTurnComplete);
		Context->Session->OnStateChanged.AddSP(this, &SCortexConversionChat::OnSessionStateChanged);
	}
}

SCortexConversionChat::~SCortexConversionChat()
{
	if (Context.IsValid() && Context->Session.IsValid())
	{
		Context->Session->OnStreamEvent.RemoveAll(this);
		Context->Session->OnTurnComplete.RemoveAll(this);
		Context->Session->OnStateChanged.RemoveAll(this);
	}
}

void SCortexConversionChat::SendMessage(const FString& Message)
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

void SCortexConversionChat::CancelRequest()
{
	if (Context.IsValid() && Context->Session.IsValid())
	{
		Context->Session->Cancel();
	}
}

void SCortexConversionChat::OnStreamEvent(const FCortexStreamEvent& Event)
{
	// Text streaming (ContentBlockDelta) is NOT shown in the chat list.
	// The full formatted response appears only on turn complete.
	if (Event.Type == ECortexStreamEventType::SystemError)
	{
		RefreshVisibleEntries();
		if (bAutoScroll)
		{
			ScrollToBottom();
		}
	}
}

void SCortexConversionChat::OnTurnComplete(const FCortexTurnResult& Result)
{
	if (!Context.IsValid() || !Context->Session.IsValid())
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
		Context->Session->ReplaceStreamingEntry(ErrorEntries);
	}
	else
	{
		// Build assistant entries from the full response
		TArray<TSharedPtr<FCortexChatEntry>> ResponseEntries = BuildAssistantEntries(Result.ResultText);

		// Process code blocks for auto-apply or Apply button rendering
		ProcessCodeBlocks(ResponseEntries);

		// Replace streaming entry with final parsed entries
		Context->Session->ReplaceStreamingEntry(ResponseEntries);
	}

	// Clear initial generation flag after first complete response
	if (Context->bIsInitialGeneration)
	{
		Context->bIsInitialGeneration = false;
	}

	RefreshVisibleEntries();
	ScrollToBottom();

	if (InputArea.IsValid())
	{
		InputArea->SetStreaming(false);
	}
}

void SCortexConversionChat::OnSessionStateChanged(const FCortexSessionStateChange& Change)
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

TArray<TSharedPtr<FCortexChatEntry>> SCortexConversionChat::BuildAssistantEntries(const FString& FullText) const
{
	TArray<TSharedPtr<FCortexChatEntry>> Entries;

	TArray<FCortexMarkdownBlock> Blocks = CortexMarkdownParser::ParseBlocks(FullText);
	for (const FCortexMarkdownBlock& Block : Blocks)
	{
		TSharedPtr<FCortexChatEntry> Entry = MakeShared<FCortexChatEntry>();

		if (Block.Type == ECortexMarkdownBlockType::CodeBlock)
		{
			Entry->Type = ECortexChatEntryType::CodeBlock;
			Entry->Text = Block.RawText;
			Entry->Language = Block.Language;
			Entry->CodeBlockTarget = Block.CodeBlockTarget;
		}
		else
		{
			Entry->Type = ECortexChatEntryType::AssistantMessage;
			Entry->Text = Block.RawText;
		}

		Entries.Add(Entry);
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

void SCortexConversionChat::ProcessCodeBlocks(const TArray<TSharedPtr<FCortexChatEntry>>& Entries)
{
	if (!Context.IsValid() || !Context->Document.IsValid())
	{
		return;
	}

	for (const TSharedPtr<FCortexChatEntry>& Entry : Entries)
	{
		if (Entry->Type != ECortexChatEntryType::CodeBlock)
		{
			continue;
		}
		if (Entry->CodeBlockTarget.IsEmpty())
		{
			continue;
		}

		// Auto-apply during initial generation
		if (Context->bIsInitialGeneration)
		{
			if (Entry->CodeBlockTarget == TEXT("header"))
			{
				Context->Document->UpdateHeader(Entry->Text);
			}
			else if (Entry->CodeBlockTarget == TEXT("implementation"))
			{
				Context->Document->UpdateImplementation(Entry->Text);
			}
			else if (Entry->CodeBlockTarget == TEXT("snippet"))
			{
				Context->Document->UpdateSnippet(Entry->Text);
			}
		}
		// For follow-up modifications, Apply buttons are added during row generation
	}
}

TSharedRef<ITableRow> SCortexConversionChat::GenerateRow(
	TSharedPtr<FCortexChatDisplayRow> Row,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	TSharedRef<SWidget> Content = SNullWidget::NullWidget;

	if (Row.IsValid() && Row->PrimaryEntry.IsValid())
	{
		switch (Row->RowType)
		{
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
		{
			const bool bHasTarget = !Row->PrimaryEntry->CodeBlockTarget.IsEmpty();
			const bool bShowApplyButtons = bHasTarget && Context.IsValid() && !Context->bIsInitialGeneration;

			if (bShowApplyButtons)
			{
				// Code block with Apply button for follow-up responses
				TSharedRef<SVerticalBox> CodeWithApply = SNew(SVerticalBox);

				CodeWithApply->AddSlot()
				.AutoHeight()
				[
					SNew(SCortexCodeBlock)
					.Code(Row->PrimaryEntry->Text)
					.Language(Row->PrimaryEntry->Language)
				];

				// Apply button based on target
				FString Target = Row->PrimaryEntry->CodeBlockTarget;
				FString ButtonLabel;
				if (Target == TEXT("header"))
				{
					ButtonLabel = TEXT("Apply to .h");
				}
				else if (Target == TEXT("implementation"))
				{
					ButtonLabel = TEXT("Apply to .cpp");
				}
				else if (Target == TEXT("snippet"))
				{
					ButtonLabel = TEXT("Apply Snippet");
				}

				FString CodeText = Row->PrimaryEntry->Text;
				TSharedPtr<FCortexConversionContext> CtxCopy = Context;

				CodeWithApply->AddSlot()
				.AutoHeight()
				.Padding(8.0f, 4.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(ButtonLabel))
					.OnClicked_Lambda([CtxCopy, Target, CodeText]()
					{
						if (CtxCopy.IsValid() && CtxCopy->Document.IsValid())
						{
							if (Target == TEXT("header"))
							{
								CtxCopy->Document->UpdateHeader(CodeText);
							}
							else if (Target == TEXT("implementation"))
							{
								CtxCopy->Document->UpdateImplementation(CodeText);
							}
							else if (Target == TEXT("snippet"))
							{
								CtxCopy->Document->UpdateSnippet(CodeText);
							}
						}
						return FReply::Handled();
					})
				];

				Content = CodeWithApply;
			}
			else
			{
				// Plain code block (initial generation or untagged)
				Content = SNew(SCortexCodeBlock)
					.Code(Row->PrimaryEntry->Text)
					.Language(Row->PrimaryEntry->Language);
			}
			break;
		}

		case ECortexChatRowType::ProcessingRow:
			break;
		}
	}

	return SNew(STableRow<TSharedPtr<FCortexChatDisplayRow>>, OwnerTable)
		.Padding(FMargin(8.0f, 4.0f))
		[
			Content
		];
}

void SCortexConversionChat::RefreshVisibleEntries()
{
	DisplayRows.Empty();

	if (!Context.IsValid() || !Context->Session.IsValid())
	{
		if (ChatList.IsValid())
		{
			ChatList->RequestListRefresh();
		}
		return;
	}

	const TArray<TSharedPtr<FCortexChatEntry>>& Entries = Context->Session->GetChatEntries();
	for (const TSharedPtr<FCortexChatEntry>& Entry : Entries)
	{
		TSharedPtr<FCortexChatDisplayRow> Row = MakeShared<FCortexChatDisplayRow>();
		Row->PrimaryEntry = Entry;

		if (Entry->Type == ECortexChatEntryType::UserMessage)
		{
			Row->RowType = ECortexChatRowType::UserMessage;
		}
		else if (Entry->Type == ECortexChatEntryType::CodeBlock)
		{
			Row->RowType = ECortexChatRowType::CodeBlock;
		}
		else
		{
			Row->RowType = ECortexChatRowType::AssistantTurn;
		}

		DisplayRows.Add(Row);
	}

	if (ChatList.IsValid())
	{
		ChatList->RequestListRefresh();
	}
}

void SCortexConversionChat::ScrollToBottom()
{
	if (ChatList.IsValid() && DisplayRows.Num() > 0)
	{
		ChatList->RequestScrollIntoView(DisplayRows.Last());
	}
}
