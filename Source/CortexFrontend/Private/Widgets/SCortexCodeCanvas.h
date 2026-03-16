#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexConversionContext.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexCodeBlock;
class SWidgetSwitcher;
class STextBlock;

DECLARE_DELEGATE(FOnCreateFilesClicked);

class SCortexCodeCanvas : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexCodeCanvas) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexCodeDocument>, Document)
		SLATE_EVENT(FOnCreateFilesClicked, OnCreateFiles)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCortexCodeCanvas();

private:
	void OnDocumentChanged(ECortexCodeTab ChangedTab);
	FReply OnCopyClicked();
	FReply OnCreateFilesButtonClicked();
	void SwitchToTab(ECortexCodeTab Tab);
	void UpdateTabLabels();

	/** Count newlines in a string to get line count. */
	static int32 CountLines(const FString& Code);

	TSharedPtr<FCortexCodeDocument> Document;
	TSharedPtr<SWidgetSwitcher> CodeSwitcher;
	TSharedPtr<SCortexCodeBlock> HeaderBlock;
	TSharedPtr<SCortexCodeBlock> ImplementationBlock;
	TSharedPtr<STextBlock> HeaderTabLabel;
	TSharedPtr<STextBlock> ImplTabLabel;
	FOnCreateFilesClicked OnCreateFilesDelegate;
	ECortexCodeTab CurrentTab = ECortexCodeTab::Header;
	FDelegateHandle DocumentChangedHandle;
};
