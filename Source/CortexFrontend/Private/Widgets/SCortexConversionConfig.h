#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexConversionContext.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

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

	TSharedRef<SWidget> BuildEventFunctionList(const FCortexConversionPayload& Payload);

	TSharedPtr<FCortexConversionContext> Context;
	FOnConvertClicked OnConvert;
};
