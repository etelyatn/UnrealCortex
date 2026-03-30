#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexConversionContext.h"
#include "Session/CortexSessionTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FMonitoredProcess;
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
	virtual ~SCortexConversionTab();

private:
	void OnConvertClicked();
	void StartConversion(const FString& AssembledSystemPrompt);
	void StatusMessage(const FString& Message);
	void OnCreateFilesRequested();
	void RunBuildVerification();
	void CancelBuild();
	void OnBuildOutputInternal(const FString& Output);
	void OnBuildCompletedInternal(int32 ReturnCode);
	void OnSessionTurnComplete(const FCortexTurnResult& Result);
	void OnSessionStateChanged(const FCortexSessionStateChange& Change);
	void SaveConversionNotes(const FString& ResponseText);

	TSharedPtr<FCortexConversionContext> Context;
	TSharedPtr<SWidgetSwitcher> ViewSwitcher;
	TSharedPtr<SCortexCodeCanvas> CodeCanvas;
	TSharedPtr<SCortexConversionChat> ConversionChat;
	TSharedPtr<FMonitoredProcess> BuildProcess;
	FString BuildOutputAccumulator;
	bool bLiveCodingWasEnabled = false;
};
