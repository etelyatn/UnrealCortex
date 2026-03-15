#pragma once

#include "CoreMinimal.h"
#include "Session/CortexCliSession.h"
#include "Session/CortexSessionTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE(FOnCortexSidebarToggle);

class SCortexSidebar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexSidebar) {}
		SLATE_ARGUMENT(TWeakPtr<FCortexCliSession>, Session)
		SLATE_EVENT(FOnCortexSidebarToggle, OnCollapse)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	~SCortexSidebar();

	void SetCollapsed(bool bCollapsed);

private:
	void OnTokenUsageUpdated();
	void OnSessionStateChanged(const FCortexSessionStateChange& Change);
	void UpdateModelDisplay();

	TWeakPtr<FCortexCliSession> SessionWeak;
	FDelegateHandle TokenUsageHandle;
	FDelegateHandle StateChangedHandle;
	FOnCortexSidebarToggle OnCollapse;
	TSharedPtr<STextBlock> CollapseButtonText;
	TSharedPtr<STextBlock> ProviderText;
	TSharedPtr<STextBlock> ModelText;
	TSharedPtr<STextBlock> StateText;
	TArray<TSharedPtr<FString>> ModelOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ModelComboBox;
	TSharedPtr<FString> SelectedModelOption;
	TArray<TSharedPtr<FString>> AccessModeOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> AccessModeComboBox;
	TSharedPtr<FString> SelectedAccessModeOption;
	TArray<TSharedPtr<FString>> EffortOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> EffortComboBox;
	TSharedPtr<FString> SelectedEffortOption;
	TSharedPtr<SSegmentedControl<ECortexWorkflowMode>> WorkflowToggle;
	TSharedPtr<SCheckBox> ProjectContextCheckbox;
	TSharedPtr<SEditableTextBox> DirectiveTextBox;
};
