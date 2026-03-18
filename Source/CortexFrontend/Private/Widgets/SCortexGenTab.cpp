#include "Widgets/SCortexGenTab.h"
#include "CortexFrontendModule.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Docking/TabManager.h"

void SCortexGenTab::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Quick Generation")))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f, 4.f)
		[
			SNew(SBox)
			.HeightOverride(60.f)
			[
				SAssignNew(PromptBox, SMultiLineEditableTextBox)
				.HintText(FText::FromString(TEXT("Enter a prompt to pre-fill in Studio...")))
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f, 4.f)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Open Studio")))
			.OnClicked(FOnClicked::CreateSP(this, &SCortexGenTab::OnOpenStudioClicked))
		]
	];
}

FReply SCortexGenTab::OnOpenStudioClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FName("CortexGenStudio"));
	return FReply::Handled();
}
