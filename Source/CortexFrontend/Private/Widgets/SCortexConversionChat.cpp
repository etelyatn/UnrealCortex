#include "Widgets/SCortexConversionChat.h"

#include "CortexFrontendModule.h"
#include "Rendering/CortexMarkdownParser.h"
#include "Session/CortexCliSession.h"
#include "Widgets/SCortexChatMessage.h"
#include "Widgets/SCortexCodeBlock.h"
#include "Widgets/SCortexDiffBlock.h"
#include "Widgets/SCortexInputArea.h"
#include "Widgets/SCortexProcessingIndicator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
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

SCortexConversionChat::~SCortexConversionChat()
{
	if (Context.IsValid() && Context->Session.IsValid())
	{
		Context->Session->OnStreamEvent.RemoveAll(this);
		Context->Session->OnTurnComplete.RemoveAll(this);
		Context->Session->OnStateChanged.RemoveAll(this);
	}
}

void SCortexConversionChat::AddStatusMessage(const FString& Message)
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

	// Clear diff apply warnings from previous interactions
	WarningRows.Empty();

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

	// Collapse status messages into a short summary
	CollapseStatusMessages(Result);

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

	// During initial generation, infer targets for any untagged C++ code blocks.
	// LLMs sometimes tag one block (e.g., implementation) but leave the other
	// untagged, or output all blocks without tags. This handles all combinations.
	if (Context->bIsInitialGeneration)
	{
		// First pass: collect tagged targets and untagged blocks
		bool bHasTaggedHeader = false;
		bool bHasTaggedImpl = false;
		bool bHasTaggedSnippet = false;
		TArray<TSharedPtr<FCortexChatEntry>> UntaggedCodeBlocks;

		for (const TSharedPtr<FCortexChatEntry>& Entry : Entries)
		{
			if (Entry->Type != ECortexChatEntryType::CodeBlock)
			{
				continue;
			}
			if (Entry->CodeBlockTarget == TEXT("header"))
			{
				bHasTaggedHeader = true;
			}
			else if (Entry->CodeBlockTarget == TEXT("implementation"))
			{
				bHasTaggedImpl = true;
			}
			else if (Entry->CodeBlockTarget == TEXT("snippet"))
			{
				bHasTaggedSnippet = true;
			}
			else if (Entry->Language == TEXT("cpp") || Entry->Language == TEXT("c++") || Entry->Language == TEXT("h"))
			{
				UntaggedCodeBlocks.Add(Entry);
			}
		}

		// Second pass: infer targets for untagged blocks, skipping targets
		// already claimed by tagged blocks
		if (UntaggedCodeBlocks.Num() > 0)
		{
			UE_LOG(LogCortexFrontend, Warning,
				TEXT("ProcessCodeBlocks: %d untagged C++ blocks found (tagged: header=%d impl=%d snippet=%d) — inferring targets"),
				UntaggedCodeBlocks.Num(),
				bHasTaggedHeader ? 1 : 0,
				bHasTaggedImpl ? 1 : 0,
				bHasTaggedSnippet ? 1 : 0);

			if (Context->Document->bIsSnippetMode)
			{
				// Snippet mode: all untagged blocks become snippets
				for (const TSharedPtr<FCortexChatEntry>& Block : UntaggedCodeBlocks)
				{
					if (!bHasTaggedSnippet)
					{
						Block->CodeBlockTarget = TEXT("snippet");
					}
				}
			}
			else
			{
				// Full-class mode: infer header vs implementation from content
				for (const TSharedPtr<FCortexChatEntry>& Block : UntaggedCodeBlocks)
				{
					const FString& Code = Block->Text;
					const bool bLooksLikeHeader = Code.Contains(TEXT("#pragma once"))
						|| Code.Contains(TEXT("UCLASS("))
						|| Code.Contains(TEXT("USTRUCT("))
						|| Code.Contains(TEXT("GENERATED_BODY()"));

					if (bLooksLikeHeader && !bHasTaggedHeader)
					{
						Block->CodeBlockTarget = TEXT("header");
						bHasTaggedHeader = true;
					}
					else if (!bHasTaggedImpl)
					{
						Block->CodeBlockTarget = TEXT("implementation");
						bHasTaggedImpl = true;
					}
					else if (!bHasTaggedHeader)
					{
						// Last resort: if implementation is claimed, try header
						Block->CodeBlockTarget = TEXT("header");
						bHasTaggedHeader = true;
					}
				}
			}
		}
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

	// In full-class mode, warn if implementation was generated but header was not.
	// This happens when the LLM ignores the tagging instructions.
	if (Context->bIsInitialGeneration && !Context->Document->bIsSnippetMode)
	{
		const bool bHasImpl = !Context->Document->ImplementationCode.IsEmpty();
		const bool bHasHeader = !Context->Document->HeaderCode.IsEmpty();

		if (bHasImpl && !bHasHeader)
		{
			UE_LOG(LogCortexFrontend, Warning,
				TEXT("ProcessCodeBlocks: Full-class mode — implementation generated but header is missing"));
			AddStatusMessage(TEXT(
				"[Warning] Header file was not generated. "
				"Ask: \"Please provide the .h file using the ```cpp:header tag.\""));
		}
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
		case ECortexChatRowType::StatusRow:
		{
			// Status messages: gray italic text with a subtle left border
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
		{
			const bool bIsFollowUp = Row->PrimaryEntry->TurnIndex > 1;
			const FString Target = Row->PrimaryEntry->CodeBlockTarget;

			// Diff block: render as unified diff view with targeted apply
			if (bIsFollowUp && Row->bIsDiffBlock && Context.IsValid())
			{
				TSharedPtr<FCortexConversionContext> CtxCopy = Context;
				TArray<FCortexFrontendSearchReplacePair> PairsCopy = Row->SearchReplacePairs;
				TWeakPtr<SCortexConversionChat> WeakSelf = StaticCastSharedRef<SCortexConversionChat>(AsShared());

				Content = SNew(SCortexDiffBlock)
					.Target(Target)
					.Pairs(Row->SearchReplacePairs)
					.OnApply_Lambda([CtxCopy, PairsCopy, Target, WeakSelf]()
					{
						if (!CtxCopy.IsValid() || !CtxCopy->Document.IsValid())
						{
							return FReply::Handled();
						}

						auto* Doc = CtxCopy->Document.Get();

						// Determine which document text to work on
						FString WorkingText;
						if (Target == TEXT("header"))
						{
							WorkingText = Doc->HeaderCode;
						}
						else if (Target == TEXT("implementation"))
						{
							WorkingText = Doc->ImplementationCode;
						}
						else if (Target == TEXT("snippet"))
						{
							WorkingText = Doc->SnippetCode;
						}

						// Normalize for reliable matching: strip \r and trailing whitespace per line
						// (AI search text may differ from stored content in invisible whitespace)
						WorkingText = CortexDiffParser::NormalizeForDiff(WorkingText);

						bool bAnyApplied = false;
						for (int32 i = 0; i < PairsCopy.Num(); ++i)
						{
							const FCortexFrontendSearchReplacePair& Pair = PairsCopy[i];
							const int32 Pos = WorkingText.Find(
								*Pair.SearchText, ESearchCase::CaseSensitive,
								ESearchDir::FromStart);

							if (Pos == INDEX_NONE)
							{
								if (auto Pinned = WeakSelf.Pin())
								{
									Pinned->AddWarningEntry(FString::Printf(
										TEXT("Could not find match for change %d — skipped"), i + 1));
								}
								continue;
							}

							// Check for ambiguity
							const int32 Pos2 = WorkingText.Find(
								*Pair.SearchText, ESearchCase::CaseSensitive,
								ESearchDir::FromStart, Pos + Pair.SearchText.Len());
							if (Pos2 != INDEX_NONE)
							{
								if (auto Pinned = WeakSelf.Pin())
								{
									Pinned->AddWarningEntry(FString::Printf(
										TEXT("Ambiguous match for change %d — skipped (search text appears multiple times)"), i + 1));
								}
								continue;
							}

							// Splice
							WorkingText = WorkingText.Left(Pos) + Pair.ReplaceText
								+ WorkingText.Mid(Pos + Pair.SearchText.Len());
							bAnyApplied = true;
						}

						if (bAnyApplied)
						{
							// Save snapshot only when we actually apply something
							Doc->SaveSnapshot();
							if (Target == TEXT("header"))
							{
								Doc->UpdateHeader(WorkingText);
							}
							else if (Target == TEXT("implementation"))
							{
								Doc->UpdateImplementation(WorkingText);
							}
							else if (Target == TEXT("snippet"))
							{
								Doc->UpdateSnippet(WorkingText);
							}
						}

						return FReply::Handled();
					})
					.OnRevert_Lambda([CtxCopy]()
					{
						if (CtxCopy.IsValid() && CtxCopy->Document.IsValid())
						{
							CtxCopy->Document->RevertToSnapshot();
						}
						return FReply::Handled();
					})
					.IsRevertVisible_Lambda([CtxCopy]()
					{
						return CtxCopy.IsValid() && CtxCopy->Document.IsValid()
							&& CtxCopy->Document->bHasSnapshot;
					});
				break;
			}

			if (bIsFollowUp && Context.IsValid())
			{
				// Follow-up code block: show with Apply button(s)
				TSharedRef<SVerticalBox> CodeWithApply = SNew(SVerticalBox);

				CodeWithApply->AddSlot()
				.AutoHeight()
				[
					SNew(SBox)
					.MaxDesiredHeight(400.0f)
					[
						SNew(SCortexCodeBlock)
						.Code(Row->PrimaryEntry->Text)
						.Language(Row->PrimaryEntry->Language)
					]
				];

				FString CodeText = Row->PrimaryEntry->Text;
				TSharedPtr<FCortexConversionContext> CtxCopy = Context;

				if (!Target.IsEmpty())
				{
					// Tagged block: single Apply button for the specified target
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
				}
				else
				{
					// Untagged block: offer both .h and .cpp Apply options
					CodeWithApply->AddSlot()
					.AutoHeight()
					.Padding(8.0f, 4.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0, 0, 4, 0)
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("Apply to .h")))
							.OnClicked_Lambda([CtxCopy, CodeText]()
							{
								if (CtxCopy.IsValid() && CtxCopy->Document.IsValid())
								{
									CtxCopy->Document->UpdateHeader(CodeText);
								}
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("Apply to .cpp")))
							.OnClicked_Lambda([CtxCopy, CodeText]()
							{
								if (CtxCopy.IsValid() && CtxCopy->Document.IsValid())
								{
									CtxCopy->Document->UpdateImplementation(CodeText);
								}
								return FReply::Handled();
							})
						]
					];
				}

				Content = CodeWithApply;
			}
			else
			{
				// Initial generation: plain code block (auto-applied to canvas)
				Content = SNew(SBox)
				.MaxDesiredHeight(400.0f)
				[
					SNew(SCortexCodeBlock)
						.Code(Row->PrimaryEntry->Text)
						.Language(Row->PrimaryEntry->Language)
				];
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

void SCortexConversionChat::CollapseStatusMessages(const FCortexTurnResult& Result)
{
	// Replace all step-by-step status rows with a single compact summary
	StatusRows.Empty();

	TSharedPtr<FCortexChatEntry> SummaryEntry = MakeShared<FCortexChatEntry>();
	SummaryEntry->Type = ECortexChatEntryType::AssistantMessage;

	if (Result.bIsError)
	{
		SummaryEntry->Text = FString::Printf(TEXT("Conversion failed (%dms): %s"),
			Result.DurationMs, *Result.ResultText);
	}
	else
	{
		FString TokenInfo;
		if (Context.IsValid() && Context->Session.IsValid())
		{
			const int64 InTokens = Context->Session->GetTotalInputTokens();
			const int64 OutTokens = Context->Session->GetTotalOutputTokens();
			if (InTokens > 0 || OutTokens > 0)
			{
				TokenInfo = FString::Printf(TEXT(" \u00B7 %lldK in / %lldK out"),
					(InTokens + 500) / 1000, (OutTokens + 500) / 1000);
			}
		}
		SummaryEntry->Text = FString::Printf(TEXT("Converted in %.1fs%s"),
			Result.DurationMs / 1000.0, *TokenInfo);
	}

	TSharedPtr<FCortexChatDisplayRow> SummaryRow = MakeShared<FCortexChatDisplayRow>();
	SummaryRow->PrimaryEntry = SummaryEntry;
	SummaryRow->RowType = ECortexChatRowType::StatusRow;
	StatusRows.Add(SummaryRow);
}

void SCortexConversionChat::RefreshVisibleEntries()
{
	DisplayRows.Empty();

	// Status messages first (serialization steps, session status, etc.)
	DisplayRows.Append(StatusRows);

	if (Context.IsValid() && Context->Session.IsValid())
	{
		const TArray<TSharedPtr<FCortexChatEntry>>& Entries = Context->Session->GetChatEntries();
		for (const TSharedPtr<FCortexChatEntry>& Entry : Entries)
		{
			// Skip the initial BP JSON user message — it's internal, not user-typed
			if (Entry->Type == ECortexChatEntryType::UserMessage && Entry->TurnIndex <= 1)
			{
				continue;
			}

			// Skip code blocks that were auto-applied to canvas during initial generation.
			// Follow-up code blocks (TurnIndex > 1) must remain visible with Apply buttons.
			if (Entry->Type == ECortexChatEntryType::CodeBlock && !Entry->CodeBlockTarget.IsEmpty() && Entry->TurnIndex <= 1)
			{
				continue;
			}

			TSharedPtr<FCortexChatDisplayRow> Row = MakeShared<FCortexChatDisplayRow>();
			Row->PrimaryEntry = Entry;

			if (Entry->Type == ECortexChatEntryType::UserMessage)
			{
				Row->RowType = ECortexChatRowType::UserMessage;
			}
			else if (Entry->Type == ECortexChatEntryType::CodeBlock)
			{
				Row->RowType = ECortexChatRowType::CodeBlock;

				// Parse diff markers for tagged follow-up code blocks
				if (!Entry->CodeBlockTarget.IsEmpty() && Entry->TurnIndex > 1)
				{
					TArray<FCortexFrontendSearchReplacePair> DiffPairs;
					if (CortexDiffParser::Parse(Entry->Text, DiffPairs))
					{
						Row->bIsDiffBlock = true;
						Row->SearchReplacePairs = MoveTemp(DiffPairs);
					}
				}
			}
			else
			{
				Row->RowType = ECortexChatRowType::AssistantTurn;
			}

			DisplayRows.Add(Row);
		}
	}

	// Append diff apply warnings (persist until next turn)
	DisplayRows.Append(WarningRows);

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

void SCortexConversionChat::AddWarningEntry(const FString& Message)
{
	TSharedPtr<FCortexChatEntry> Entry = MakeShared<FCortexChatEntry>();
	Entry->Type = ECortexChatEntryType::AssistantMessage;
	Entry->Text = Message;

	TSharedPtr<FCortexChatDisplayRow> Row = MakeShared<FCortexChatDisplayRow>();
	Row->PrimaryEntry = Entry;
	Row->RowType = ECortexChatRowType::StatusRow;

	WarningRows.Add(Row);

	RefreshVisibleEntries();
	ScrollToBottom();
}
