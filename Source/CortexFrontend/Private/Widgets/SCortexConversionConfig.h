#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexConversionContext.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SButton;
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

	TSharedRef<SWidget> BuildScopeAndTargetSection(const FCortexConversionPayload& Payload);
	TSharedRef<SWidget> BuildInstructionsSection();

	void OnDepthChanged(ECortexConversionDepth NewDepth);
	bool IsDepthSelected(ECortexConversionDepth Depth) const;
	ECortexConversionDepth DefaultDepthForScope(ECortexConversionScope Scope) const;

	void OnDestinationChanged(ECortexConversionDestination NewDest);
	bool IsDestinationSelected(ECortexConversionDestination Dest) const;
	void OnTargetAncestorSelected(int32 AncestorIndex);
	TSharedRef<SWidget> BuildDestinationSection(const FCortexConversionPayload& Payload);
	TSharedRef<SWidget> BuildWarningBars(const FCortexConversionPayload& Payload);

	TSharedPtr<FCortexConversionContext> Context;
	TSharedPtr<SMultiLineEditableTextBox> CustomInstructionsBox;
	TSharedPtr<SCortexScopeSelector> ScopeSelector;
	FOnConvertClicked OnConvert;

	void UpdateCustomInstructionsVisibility();
	void RequestTokenEstimate();
	int32 EstimateTokensForScope(ECortexConversionScope Scope) const;
	FString FormatTokenEstimate(int32 Tokens) const;
	static FString FormatTokenCount(int32 Tokens);

	TSharedPtr<STextBlock> ConvertButtonText;
	TSharedPtr<STextBlock> TokenWarningText;
	TSharedPtr<SButton> ConvertButton;

	static constexpr int32 SoftTokenLimit = 40000;
	static constexpr int32 HardTokenLimit = 80000;
};
