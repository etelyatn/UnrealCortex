#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SMultiLineEditableTextBox;
class STextBlock;
class SButton;
class STextComboBox;

class SCortexGenTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexGenTab) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCortexGenTab();

private:
	void OnDomainProgress(const FName& DomainName, const TSharedPtr<FJsonObject>& Data);
	FReply OnGenerateClicked();
	void ExecuteGenCommand(const FString& Command, TSharedPtr<FJsonObject> Params);

	void OnQualityChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo);

	TSharedPtr<SMultiLineEditableTextBox> PromptBox;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<STextComboBox> GenerationTypeCombo;
	TSharedPtr<STextComboBox> QualityCombo;

	// Combo box options
	TArray<TSharedPtr<FString>> GenerationTypeOptions;
	TArray<TSharedPtr<FString>> QualityOptions;

	// Current quality selection (generation type resolved from combo pointer)
	FString SelectedQuality = TEXT("medium");

	FDelegateHandle DomainProgressHandle;
};
