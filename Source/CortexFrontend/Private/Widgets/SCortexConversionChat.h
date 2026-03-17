#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexConversionContext.h"
#include "Process/CortexStreamEvent.h"
#include "Session/CortexCliSession.h"
#include "Widgets/CortexDisplayTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexInputArea;
class STableViewBase;

template <typename ItemType>
class SListView;

class SCortexConversionChat : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexConversionChat) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexConversionContext>, Context)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCortexConversionChat();

	/** Bind to session after it's created (called from SCortexConversionTab::StartConversion). */
	void BindSession();

	/** Add a status message to the chat (shown as a step in the conversion process). */
	void AddStatusMessage(const FString& Message);

private:
	void SendMessage(const FString& Message);
	void CancelRequest();
	void OnStreamEvent(const FCortexStreamEvent& Event);
	void OnTurnComplete(const FCortexTurnResult& Result);
	void OnSessionStateChanged(const FCortexSessionStateChange& Change);

	TSharedRef<ITableRow> GenerateRow(
		TSharedPtr<FCortexChatDisplayRow> Row,
		const TSharedRef<STableViewBase>& OwnerTable);

	TArray<TSharedPtr<FCortexChatEntry>> BuildAssistantEntries(const FString& FullText) const;
	void ProcessCodeBlocks(const TArray<TSharedPtr<FCortexChatEntry>>& Entries);
	void CollapseStatusMessages(const FCortexTurnResult& Result);
	void RefreshVisibleEntries();
	void ScrollToBottom();

	void AddWarningEntry(const FString& Message);

	TSharedPtr<FCortexConversionContext> Context;
	TSharedPtr<SCortexInputArea> InputArea;
	TSharedPtr<SListView<TSharedPtr<FCortexChatDisplayRow>>> ChatList;
	TSharedPtr<SBox> ProcessingIndicatorBox;

	/** Status messages pushed before/during session creation. */
	TArray<TSharedPtr<FCortexChatDisplayRow>> StatusRows;

	/** Warnings from diff apply failures — appended to DisplayRows during RefreshVisibleEntries, cleared on next turn. */
	TArray<TSharedPtr<FCortexChatDisplayRow>> WarningRows;

	TArray<TSharedPtr<FCortexChatDisplayRow>> DisplayRows;
	bool bAutoScroll = true;
};
