#pragma once

#include "CoreMinimal.h"
#include "CortexConversionTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;

DECLARE_DELEGATE_OneParam(FOnScopeChanged, ECortexConversionScope);
DECLARE_DELEGATE_TwoParams(FOnFunctionToggled, const FString& /*Name*/, bool /*bChecked*/);

class SCortexScopeSelector : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexScopeSelector) {}
		SLATE_ARGUMENT(ECortexConversionScope, InitialScope)
		SLATE_ARGUMENT(FString, CurrentGraphName)
		SLATE_ARGUMENT(TArray<FString>, EventNames)
		SLATE_ARGUMENT(TArray<FString>, FunctionNames)
		SLATE_ARGUMENT(TArray<FString>, GraphNames)
		SLATE_ARGUMENT(int32, SelectedNodeCount)
		SLATE_EVENT(FOnScopeChanged, OnScopeChanged)
		SLATE_EVENT(FOnFunctionToggled, OnFunctionToggled)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	ECortexConversionScope GetSelectedScope() const { return CurrentScope; }
	TArray<FString> GetSelectedFunctions() const;

	void SetTokenEstimates(int32 TotalTokens, const TMap<FString, int32>& PerFunctionTokens);

private:
	void OnScopeRadioChanged(ECortexConversionScope NewScope);
	bool IsScopeSelected(ECortexConversionScope Scope) const;
	void OnFunctionCheckChanged(const FString& Name, bool bChecked);
	void UpdateChecklistVisibility();
	static FString FormatTokenCount(int32 Tokens);

	ECortexConversionScope CurrentScope = ECortexConversionScope::EntireBlueprint;
	TArray<FString> EventNames;
	TArray<FString> FunctionNames;
	FString CurrentGraphName;
	TArray<FString> GraphNames;
	int32 SelectedNodeCount = 0;
	TSet<FString> CheckedFunctions;

	FOnScopeChanged OnScopeChangedDelegate;
	FOnFunctionToggled OnFunctionToggledDelegate;

	TSharedPtr<SVerticalBox> EventFunctionChecklist;

	int32 TotalTokenEstimate = 0;
	TMap<FString, int32> FunctionTokenEstimates;

	static constexpr int32 SoftTokenLimit = 40000;
	static constexpr int32 HardTokenLimit = 80000;
};
