// SCortexAnalysisConfig.h
#pragma once

#include "CoreMinimal.h"
#include "Analysis/CortexAnalysisContext.h"
#include "Analysis/CortexFindingTypes.h"
#include "Utilities/CortexTokenUtils.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SButton;
class SMultiLineEditableTextBox;
class SCortexScopeSelector;

DECLARE_DELEGATE(FOnAnalyzeClicked);

class SCortexAnalysisConfig : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexAnalysisConfig) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexAnalysisContext>, Context)
		SLATE_EVENT(FOnAnalyzeClicked, OnAnalyze)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnAnalyzeButtonClicked();

	TSharedRef<SWidget> BuildBlueprintInfoSection();
	TSharedRef<SWidget> BuildPreScanSection();
	TSharedRef<SWidget> BuildFocusCheckboxes();
	TSharedRef<SWidget> BuildDepthSelector();
	TSharedRef<SWidget> BuildCustomInstructions();

	void OnFocusToggled(ECortexFindingCategory Category, ECheckBoxState NewState);
	void OnDepthChanged(ECortexAnalysisDepth NewDepth);
	void UpdateFocusCheckboxesForDepth(ECortexAnalysisDepth Depth);

	void OnScopeChanged(ECortexConversionScope NewScope);
	void OnFunctionToggled(const FString& Name, bool bChecked);
	void RequestTokenEstimate();

	int32 EstimateTokensForScope(ECortexConversionScope Scope) const;
	FString FormatAnalysisTimeEstimate(int32 Tokens) const;

	TSharedPtr<FCortexAnalysisContext> Context;
	FOnAnalyzeClicked OnAnalyze;
	TSharedPtr<SButton> AnalyzeButton;
	TSharedPtr<STextBlock> TokenEstimateText;
	TSharedPtr<STextBlock> TokenWarningText;
	TSharedPtr<SMultiLineEditableTextBox> CustomInstructionsBox;
	TSharedPtr<SCortexScopeSelector> ScopeSelector;

	TSet<ECortexFindingCategory> EnabledFocusAreas;
	ECortexAnalysisDepth CurrentDepth = ECortexAnalysisDepth::Standard;
};
