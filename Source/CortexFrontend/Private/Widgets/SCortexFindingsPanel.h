// Source/CortexFrontend/Private/Widgets/SCortexFindingsPanel.h
#pragma once

#include "CoreMinimal.h"
#include "Analysis/CortexAnalysisContext.h"
#include "Analysis/CortexFindingTypes.h"
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

    /** Add a finding to the panel. Called by analysis chat as findings stream in. */
    void AddFinding(const FCortexAnalysisFinding& Finding);

    /** Clear all findings (on re-analysis). */
    void ClearFindings();

    /** Refresh the list view. */
    void RequestRefresh();

private:
    TSharedRef<ITableRow> GenerateRow(
        TSharedPtr<FCortexAnalysisFinding> Finding,
        const TSharedRef<STableViewBase>& OwnerTable);

    void OnFindingClicked(TSharedPtr<FCortexAnalysisFinding> Finding);
    FText GetSummaryText() const;
    FSlateColor GetSeverityColor(ECortexFindingSeverity Severity) const;
    FText GetCategoryLabel(ECortexFindingCategory Category) const;

    TSharedPtr<FCortexAnalysisContext> Context;
    FOnFindingSelected OnFindingSelectedDelegate;
    TSharedPtr<SListView<TSharedPtr<FCortexAnalysisFinding>>> FindingsList;
    TArray<TSharedPtr<FCortexAnalysisFinding>> FindingsData;
    TSharedPtr<STextBlock> SummaryText;
};
