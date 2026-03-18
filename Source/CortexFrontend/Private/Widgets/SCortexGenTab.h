#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SMultiLineEditableTextBox;
class STextBlock;
class SButton;

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
	void OnCancelClicked(const FString& JobId);
	void ExecuteGenCommand(const FString& Command, TSharedPtr<FJsonObject> Params);
	void RefreshProviders();
	void RefreshJobs();

	TSharedPtr<SMultiLineEditableTextBox> PromptBox;
	TSharedPtr<STextBlock> StatusText;

	FDelegateHandle DomainProgressHandle;
};
