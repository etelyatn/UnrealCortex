// SCortexAnalysisConfig.h
#pragma once

#include "CoreMinimal.h"
#include "Analysis/CortexAnalysisContext.h"
#include "Analysis/CortexFindingTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SButton;
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

	void OnFocusToggled(ECortexFindingCategory Category, ECheckBoxState NewState);

	void OnScopeChanged(ECortexConversionScope NewScope);
	void OnFunctionToggled(const FString& Name, bool bChecked);
	void RequestTokenEstimate();

	TSharedPtr<FCortexAnalysisContext> Context;
	FOnAnalyzeClicked OnAnalyze;
	TSharedPtr<SButton> AnalyzeButton;
	TSharedPtr<SCortexScopeSelector> ScopeSelector;

	TSet<ECortexFindingCategory> EnabledFocusAreas;
};
