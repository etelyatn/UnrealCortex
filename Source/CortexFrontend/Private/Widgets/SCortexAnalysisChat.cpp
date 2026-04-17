// SCortexAnalysisChat.cpp
#include "Widgets/SCortexAnalysisChat.h"

#include "Analysis/CortexFindingTypes.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Regex.h"
#include "Rendering/CortexChatEntryBuilder.h"
#include "Widgets/SCortexChatMessage.h"
#include "Widgets/SCortexChatPanel.h"
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
	OnAnalysisSummaryDelegate = InArgs._OnAnalysisSummary;

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
	RefreshVisibleEntries();
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

	// Extract findings from the assistant response — even on error,
	// partial findings streamed before the crash are worth preserving
	{
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

/**
 * Replace all "node_N" references in text with "'DisplayName' (node_N)" using
 * the node display name mapping from the analysis context.
 * E.g., "node_8" → "'Select Float' (node_8)"
 */
static FString ResolveNodeReferencesInText(
	const FString& Text,
	const FCortexAnalysisContext& Context)
{
	if (Text.IsEmpty() || Context.NodeDisplayNames.Num() == 0)
	{
		return Text;
	}

	// Match standalone "node_N" tokens (word boundary prevents node_1 matching inside node_12)
	static const FRegexPattern Pattern(TEXT("\\bnode_(\\d+)\\b"));
	FRegexMatcher Matcher(Pattern, Text);

	FString Result;
	int32 LastPos = 0;

	while (Matcher.FindNext())
	{
		const int32 MatchBegin = Matcher.GetMatchBeginning();
		const int32 MatchEnd = Matcher.GetMatchEnding();

		// Append text before this match
		Result += Text.Mid(LastPos, MatchBegin - LastPos);

		// Extract and resolve the node ID
		const FString IdStr = Matcher.GetCaptureGroup(1);
		if (IdStr.IsNumeric())
		{
			const int32 NodeId = FCString::Atoi(*IdStr);
			const FString DisplayName = Context.GetNodeDisplayName(NodeId);

			// If resolved to an actual name (not fallback "node_N"), show both
			if (!DisplayName.StartsWith(TEXT("node_")))
			{
				Result += FString::Printf(TEXT("'%s' (node_%d)"), *DisplayName, NodeId);
			}
			else
			{
				// No mapping — keep original
				Result += Text.Mid(MatchBegin, MatchEnd - MatchBegin);
			}
		}
		else
		{
			Result += Text.Mid(MatchBegin, MatchEnd - MatchBegin);
		}

		LastPos = MatchEnd;
	}

	// Append remainder
	Result += Text.Mid(LastPos);
	return Result;
}

void SCortexAnalysisChat::ProcessFindings(const FString& FullText)
{
	if (!Context.IsValid())
	{
		return;
	}

	FCortexAnalysisSummary Summary;
	TArray<FCortexAnalysisFinding> NewFindings;
	FCortexChatEntryBuilder::BuildEntries(FullText, &NewFindings, &Summary);

	if (Summary.Reported > 0 || Summary.EstimatedSuppressed > 0)
	{
		OnAnalysisSummaryDelegate.ExecuteIfBound(Summary);
	}

	for (FCortexAnalysisFinding& Finding : NewFindings)
	{
		// Resolve "node_N" reference from NodeDisplayName placeholder
		if (Finding.NodeDisplayName.StartsWith(TEXT("node_")))
		{
			const FString IdStr = Finding.NodeDisplayName.Mid(5);
			// Guard against malformed LLM output like "node_abc" — FCString::Atoi returns 0
			// for non-numeric strings, which would silently resolve to node_0.
			if (!IdStr.IsNumeric())
			{
				continue;
			}
			const int32 NodeId = FCString::Atoi(*IdStr);

			FGuid ResolvedGuid;
			if (Context->ResolveNodeId(NodeId, ResolvedGuid))
			{
				Finding.NodeGuid = ResolvedGuid;
				Finding.NodeDisplayName = Context->GetNodeDisplayName(NodeId);
				Finding.GraphName = Context->GetNodeGraphName(NodeId);
			}
		}

		// Safety net: the AI prompt includes a NODE LEGEND to encourage natural names,
		// but LLMs may still emit raw node_N references in free text
		Finding.Description = ResolveNodeReferencesInText(Finding.Description, *Context);
		Finding.SuggestedFix = ResolveNodeReferencesInText(Finding.SuggestedFix, *Context);

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

	// Use wall-clock time (same as overlay) instead of CLI-reported DurationMs
	const double WallClockSeconds = (Context.IsValid() && Context->AnalysisStartTime > 0.0)
		? (FPlatformTime::Seconds() - Context->AnalysisStartTime)
		: Result.DurationMs / 1000.0;

	if (Result.bIsError && SCortexChatPanel::IsAuthError(Result.ResultText))
	{
		SummaryEntry->Type = ECortexChatEntryType::AuthError;
		SummaryEntry->Text = Result.ResultText;
	}
	else if (Result.bIsError)
	{
		SummaryEntry->Text = FString::Printf(TEXT("Analysis failed (%.1fs): %s"),
			WallClockSeconds, *Result.ResultText);
	}
	else
	{
		// Include token usage from session
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

		SummaryEntry->Text = FString::Printf(TEXT("Analysis completed in %.1fs%s"),
			WallClockSeconds, *TokenInfo);
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

			// For assistant messages, strip finding/summary code blocks — they go
			// to the findings panel, not the chat display
			if (Entry->Type == ECortexChatEntryType::AssistantMessage)
			{
				TSharedPtr<FCortexChatEntry> FilteredEntry = MakeShared<FCortexChatEntry>(*Entry);
				FilteredEntry->Text = FCortexChatEntryBuilder::StripFindingBlocks(Entry->Text);

				// Skip entirely if stripping removed all content
				if (FilteredEntry->Text.TrimStartAndEnd().IsEmpty())
				{
					continue;
				}
				Row->PrimaryEntry = FilteredEntry;
			}
			else
			{
				Row->PrimaryEntry = Entry;
			}

			if (Entry->Type == ECortexChatEntryType::AuthError)
			{
				Row->RowType = ECortexChatRowType::AuthError;
			}
			else
			{
				Row->RowType = (Entry->Type == ECortexChatEntryType::UserMessage)
					? ECortexChatRowType::UserMessage
					: ECortexChatRowType::AssistantTurn;
			}
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

		case ECortexChatRowType::AuthError:
		{
			const FString AuthCommand = (Context.IsValid() && Context->Session.IsValid())
				? Context->Session->GetAuthCommandText()
				: TEXT("login");
			Content = SNew(SCortexChatMessage)
				.Message(FString::Printf(TEXT("Authentication error: %s\n\nRun `%s` in your terminal to authenticate."),
					*Row->PrimaryEntry->Text,
					*AuthCommand))
				.IsUser(false);
			break;
		}

		case ECortexChatRowType::CodeBlock:
		case ECortexChatRowType::ProcessingRow:
		case ECortexChatRowType::TableBlock:  // TODO: SCortexTableBlock rendering not yet supported in analysis view
		{
			break;
		}
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

void SCortexAnalysisChat::ScrollToBottom()
{
	if (ChatList.IsValid() && DisplayRows.Num() > 0 && bAutoScroll)
	{
		ChatList->RequestScrollIntoView(DisplayRows.Last());
	}
}
