#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexConversionContext.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexCodeBlock;
class SCortexConversionOverlay;
class SCortexInheritedDiffView;
class SScrollBox;
class STextBlock;
class SVerticalBox;
class SWidgetSwitcher;

DECLARE_DELEGATE(FOnCreateFilesClicked);
DECLARE_DELEGATE(FOnCancelBuild);

enum class ECortexBuildStatus : uint8
{
	Hidden,
	Building,
	Succeeded,
	Failed
};

class SCortexCodeCanvas : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexCodeCanvas) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexCodeDocument>, Document)
		SLATE_ARGUMENT(TSharedPtr<FCortexConversionContext>, ConversionContext)
		SLATE_EVENT(FOnCreateFilesClicked, OnCreateFiles)
		SLATE_EVENT(FOnCancelBuild, OnCancelBuild)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCortexCodeCanvas();

	/** Show/hide the animated processing overlay. */
	void SetProcessing(bool bProcessing);

	/** Forward the serialized token count to the overlay so it can display an ETA. */
	void SetTokenCount(int32 Tokens);

	/** Update the build status bar at the bottom of the canvas. */
	void SetBuildStatus(ECortexBuildStatus Status, const FString& ErrorLog = FString());

private:
	void OnDocumentChanged(ECortexCodeTab ChangedTab);
	FReply OnCopyClicked();
	FReply OnCreateFilesButtonClicked();
	void UpdateLabels();

	static int32 CountLines(const FString& Code);

	TSharedPtr<FCortexCodeDocument> Document;
	TSharedPtr<FCortexConversionContext> ConversionContext;
	TSharedPtr<SCortexInheritedDiffView> HeaderDiffView;
	TSharedPtr<SCortexInheritedDiffView> ImplDiffView;
	TSharedPtr<SCortexCodeBlock> HeaderBlock;
	TSharedPtr<SCortexCodeBlock> ImplementationBlock;
	TSharedPtr<SCortexCodeBlock> SnippetBlock;
	TSharedPtr<STextBlock> HeaderLabel;
	TSharedPtr<STextBlock> ImplLabel;
	TSharedPtr<STextBlock> SnippetLabel;
	TSharedPtr<SCortexConversionOverlay> ProcessingOverlay;
	TSharedPtr<SWidgetSwitcher> ModeSwitcher;
	FOnCreateFilesClicked OnCreateFilesDelegate;
	FOnCancelBuild OnCancelBuildDelegate;
	FDelegateHandle DocumentChangedHandle;
	bool bIsProcessing = false;

	ECortexBuildStatus BuildStatus = ECortexBuildStatus::Hidden;
	FString BuildErrorLog;
	TSharedPtr<SVerticalBox> StatusBar;
	bool bErrorLogExpanded = false;
};
