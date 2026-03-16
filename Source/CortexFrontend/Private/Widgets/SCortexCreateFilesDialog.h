#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexConversionContext.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexCreateFilesDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexCreateFilesDialog) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexCodeDocument>, Document)
		SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Show the dialog as a modal window. Returns true if files were created. */
	static bool ShowModal(TSharedPtr<FCortexCodeDocument> Document, TSharedPtr<SWindow> ParentWindow);

private:
	FReply OnCreateClicked();
	FReply OnCancelClicked();
	bool WriteFile(const FString& Path, const FString& Content);

	TSharedPtr<FCortexCodeDocument> Document;
	TSharedPtr<SWindow> DialogWindow;

	// Snapshotted code at dialog open time
	FString SnapshotHeader;
	FString SnapshotImpl;

	// Editable fields
	FString ClassName;
	FString HeaderPath;
	FString ImplPath;
	bool bFilesCreated = false;
};
