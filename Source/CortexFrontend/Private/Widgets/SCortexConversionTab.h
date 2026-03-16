#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexConversionContext.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexCodeCanvas;
class SCortexConversionChat;
class SWidgetSwitcher;

class SCortexConversionTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexConversionTab) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexConversionContext>, Context)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	void OnConvertClicked();
	void StartConversion();
	void StatusMessage(const FString& Message);
	void OnCreateFilesRequested();

	TSharedPtr<FCortexConversionContext> Context;
	TSharedPtr<SWidgetSwitcher> ViewSwitcher;
	TSharedPtr<SCortexCodeCanvas> CodeCanvas;
	TSharedPtr<SCortexConversionChat> ConversionChat;
};
