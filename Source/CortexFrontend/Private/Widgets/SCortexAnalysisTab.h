// SCortexAnalysisTab.h
#pragma once

#include "CoreMinimal.h"
#include "Analysis/CortexAnalysisContext.h"
#include "Analysis/CortexFindingTypes.h"
#include "Rendering/CortexChatEntryBuilder.h"
#include "Session/CortexSessionTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexAnalysisConfig;
class SCortexGraphPreview;
class SCortexFindingsPanel;
class SCortexAnalysisChat;
class SWidgetSwitcher;
class SBorder;

class SCortexAnalysisTab : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexAnalysisTab) {}
        SLATE_ARGUMENT(TSharedPtr<FCortexAnalysisContext>, Context)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SCortexAnalysisTab();

private:
    void OnAnalyzeClicked();
    void StartAnalysis(const FString& AssembledSystemPrompt);
    void OnSessionTurnComplete(const FCortexTurnResult& Result);
    void OnFindingSelected(const FCortexAnalysisFinding& Finding);
    void OnNewFinding(const FCortexAnalysisFinding& Finding);
    void OnAnalysisSummary(const FCortexAnalysisSummary& Summary);
    void StatusMessage(const FString& Message);

    // Re-analysis
    void OnReAnalyzeClicked();
    void OnBlueprintCompiled(UBlueprint* Blueprint);

    TSharedPtr<FCortexAnalysisContext> Context;
    TSharedPtr<SWidgetSwitcher> ViewSwitcher;
    TSharedPtr<SCortexGraphPreview> GraphPreview;
    TSharedPtr<SCortexFindingsPanel> FindingsPanel;
    TSharedPtr<SCortexAnalysisChat> AnalysisChat;
    TSharedPtr<SBorder> RecompileBanner;
    FDelegateHandle BlueprintCompiledHandle;
};
