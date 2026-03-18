// Source/CortexFrontend/Private/Widgets/SCortexFindingsPanel.h
#pragma once

#include "CoreMinimal.h"
#include "Analysis/CortexFindingTypes.h"
#include "Rendering/CortexChatEntryBuilder.h"

struct FCortexAnalysisContext;
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class STableViewBase;

DECLARE_DELEGATE_OneParam(FOnFindingSelected, const FCortexAnalysisFinding& /*Finding*/);

class SCortexFindingsPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexFindingsPanel) {}
        SLATE_ARGUMENT(TSharedPtr<FCortexAnalysisContext>, Context)
        SLATE_EVENT(FOnFindingSelected, OnFindingSelected)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SCortexFindingsPanel();

    /** Add a finding to the panel. Called by analysis chat as findings stream in. */
    void AddFinding(const FCortexAnalysisFinding& Finding);

    /** Clear all findings (on re-analysis). */
    void ClearFindings();

    /** Refresh the list view. */
    void RequestRefresh();

    /** Update the summary header with suppression info. Called when analysis:summary is parsed. */
    void SetSummary(const FCortexAnalysisSummary& Summary);

private:
    TSharedRef<ITableRow> GenerateRow(
        TSharedPtr<FCortexAnalysisFinding> Finding,
        const TSharedRef<STableViewBase>& OwnerTable);

    void OnSelectionChanged(TSharedPtr<FCortexAnalysisFinding> Finding, ESelectInfo::Type SelectionType);
    void OnFindingClicked(TSharedPtr<FCortexAnalysisFinding> Finding);
    FText GetSummaryText() const;
    FSlateColor GetSeverityColor(ECortexFindingSeverity Severity) const;
    FText GetCategoryLabel(ECortexFindingCategory Category) const;
    TSharedRef<SWidget> BuildDetailSection(const FCortexAnalysisFinding& Finding) const;

    TSharedPtr<FCortexAnalysisContext> Context;
    FOnFindingSelected OnFindingSelectedDelegate;
    TSharedPtr<SListView<TSharedPtr<FCortexAnalysisFinding>>> FindingsList;
    TArray<TSharedPtr<FCortexAnalysisFinding>> FindingsData;
    TSharedPtr<STextBlock> SummaryText;

    FTimerHandle RefreshTimerHandle;
    bool bRefreshPending = false;

    FString ExpandedFindingKey;       // Empty = none expanded. Tracks by dedup key, not index.
    FString SummarySuppressionText;   // "~7 suppressed"
};
