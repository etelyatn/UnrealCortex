// Source/CortexFrontend/Private/Widgets/SCortexFindingsPanel.h
#pragma once

#include "CoreMinimal.h"
#include "Analysis/CortexFindingTypes.h"
#include "Rendering/CortexChatEntryBuilder.h"

struct FCortexAnalysisContext;
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SScrollBox;

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

    void AddFinding(const FCortexAnalysisFinding& Finding);
    void ClearFindings();
    void RequestRefresh();
    void SetSummary(const FCortexAnalysisSummary& Summary);

private:
    void RebuildList();
    TSharedRef<SWidget> BuildFindingCard(TSharedPtr<FCortexAnalysisFinding> Finding);
    void OnFindingClicked(TSharedPtr<FCortexAnalysisFinding> Finding);
    FText GetSummaryText() const;
    FSlateColor GetSeverityColor(ECortexFindingSeverity Severity) const;
    FText GetSeverityLabel(ECortexFindingSeverity Severity) const;
    TSharedRef<SWidget> BuildDetailSection(const FCortexAnalysisFinding& Finding) const;

    TSharedPtr<FCortexAnalysisContext> Context;
    FOnFindingSelected OnFindingSelectedDelegate;
    TSharedPtr<SScrollBox> FindingsScrollBox;
    TArray<TSharedPtr<FCortexAnalysisFinding>> FindingsData;
    TSharedPtr<STextBlock> SummaryText;

    FTimerHandle RefreshTimerHandle;
    bool bRefreshPending = false;

    FString ExpandedFindingKey;
    FString SummarySuppressionText;
};
