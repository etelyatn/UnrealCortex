#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexConversionContext.h"
#include "Utilities/CortexTokenUtils.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SCortexDependencyPanel.h"

class SButton;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class SCortexScopeSelector;

DECLARE_DELEGATE(FOnConvertClicked);

class SCortexConversionConfig : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexConversionConfig) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexConversionContext>, Context)
		SLATE_EVENT(FOnConvertClicked, OnConvert)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnConvertButtonClicked();
	void OnScopeChanged(ECortexConversionScope NewScope);
	void OnFunctionToggled(const FString& Name, bool bChecked);

	TSharedRef<SWidget> BuildTargetClassSection(const FCortexConversionPayload& Payload);
	void OnClassNameChanged(const FText& NewText);
	FText GetClassNameWarningText() const;

	TSharedRef<SWidget> BuildScopeAndTargetSection(const FCortexConversionPayload& Payload);
	TSharedRef<SWidget> BuildInstructionsSection();
	TSharedRef<SWidget> BuildWidgetBindingsSection(const FCortexConversionPayload& Payload);
	void OnWidgetBindingToggled(const FString& Name, bool bChecked);

	void OnDepthChanged(ECortexConversionDepth NewDepth);
	bool IsDepthSelected(ECortexConversionDepth Depth) const;
	ECortexConversionDepth DefaultDepthForScope(ECortexConversionScope Scope) const;

	void OnDestinationChanged(ECortexConversionDestination NewDest);
	bool IsDestinationSelected(ECortexConversionDestination Dest) const;
	void OnTargetAncestorSelected(int32 AncestorIndex);
	TSharedRef<SWidget> BuildDestinationSection(const FCortexConversionPayload& Payload);
	TSharedRef<SWidget> BuildWarningBars(const FCortexConversionPayload& Payload);

	TSharedPtr<FCortexConversionContext> Context;
	TSharedPtr<SEditableTextBox> ClassNameTextBox;
	TSharedPtr<SMultiLineEditableTextBox> CustomInstructionsBox;
	TSharedPtr<SCortexScopeSelector> ScopeSelector;
	TSharedPtr<SVerticalBox> WidgetBindingsChecklist;
	FOnConvertClicked OnConvert;

	void UpdateCustomInstructionsVisibility();
	void RequestTokenEstimate();
	int32 EstimateTokensForScope(ECortexConversionScope Scope) const;
	FString FormatTokenEstimate(int32 Tokens) const;

	TSharedPtr<STextBlock> ClassNameWarningText;
	TSharedPtr<STextBlock> ConvertButtonText;
	TSharedPtr<STextBlock> TokenWarningText;
	TSharedPtr<SButton> ConvertButton;
};
