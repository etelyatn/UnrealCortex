#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SMultiLineEditableTextBox;

class SCortexGenTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexGenTab) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnOpenStudioClicked();

	TSharedPtr<SMultiLineEditableTextBox> PromptBox;
};
