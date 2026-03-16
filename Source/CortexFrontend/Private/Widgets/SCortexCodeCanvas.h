#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexConversionContext.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexCodeBlock;
class SWidgetSwitcher;

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

	TSharedPtr<FCortexCodeDocument> Document;
	TSharedPtr<SWidgetSwitcher> CodeSwitcher;
	TSharedPtr<SCortexCodeBlock> HeaderBlock;
	TSharedPtr<SCortexCodeBlock> ImplementationBlock;
	FOnCreateFilesClicked OnCreateFilesDelegate;
	ECortexCodeTab CurrentTab = ECortexCodeTab::Header;
	FDelegateHandle DocumentChangedHandle;
};
