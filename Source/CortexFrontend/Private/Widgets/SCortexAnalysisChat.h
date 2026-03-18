// SCortexAnalysisChat.h
#pragma once

#include "CoreMinimal.h"
#include "Analysis/CortexAnalysisContext.h"
#include "Process/CortexStreamEvent.h"
#include "Rendering/CortexChatEntryBuilder.h"
#include "Session/CortexCliSession.h"
#include "Session/CortexSessionTypes.h"
#include "Widgets/CortexDisplayTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexInputArea;
class STableViewBase;

template <typename ItemType>
class SListView;

DECLARE_DELEGATE_OneParam(FOnNewFinding, const FCortexAnalysisFinding&);
DECLARE_DELEGATE_OneParam(FOnAnalysisSummary, const FCortexAnalysisSummary&);

class SCortexAnalysisChat : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexAnalysisChat) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexAnalysisContext>, Context)
		SLATE_EVENT(FOnNewFinding, OnNewFinding)
		SLATE_EVENT(FOnAnalysisSummary, OnAnalysisSummary)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCortexAnalysisChat();

	/** Bind to session after it's created. */
	void BindSession();

	/** Add a status message (step progress). */
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

	void ProcessFindings(const FString& FullText);
	void CollapseStatusMessages(const FCortexTurnResult& Result);
	void RefreshVisibleEntries();
	void ScrollToBottom();

	TSharedPtr<FCortexAnalysisContext> Context;
	FOnNewFinding OnNewFindingDelegate;
	FOnAnalysisSummary OnAnalysisSummaryDelegate;
	TSharedPtr<SCortexInputArea> InputArea;
	TSharedPtr<SListView<TSharedPtr<FCortexChatDisplayRow>>> ChatList;
	TSharedPtr<SBox> ProcessingIndicatorBox;

	TArray<TSharedPtr<FCortexChatDisplayRow>> StatusRows;
	TArray<TSharedPtr<FCortexChatDisplayRow>> DisplayRows;
	bool bAutoScroll = true;
};
