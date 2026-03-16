#include "Widgets/SCortexCreateFilesDialog.h"

#include "CortexFrontendModule.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	/** Strip single-letter UE type prefix (A, U, F, S, E, I, T) for file naming. */
	FString StripUETypePrefix(const FString& InName)
	{
		if (InName.Len() >= 2 && FChar::IsUpper(InName[0]) && FChar::IsUpper(InName[1]))
		{
			const TCHAR Prefix = InName[0];
			if (Prefix == TEXT('A') || Prefix == TEXT('U') || Prefix == TEXT('F')
				|| Prefix == TEXT('S') || Prefix == TEXT('E') || Prefix == TEXT('I')
				|| Prefix == TEXT('T'))
			{
				return InName.Mid(1);
			}
		}
		return InName;
	}
}

bool SCortexCreateFilesDialog::ShowModal(TSharedPtr<FCortexCodeDocument> Document, TSharedPtr<SWindow> ParentWindow)
{
	TSharedRef<SWindow> DialogWindow = SNew(SWindow)
		.Title(NSLOCTEXT("CortexCreateFiles", "Title", "Create C++ Files"))
		.ClientSize(FVector2D(500, 300))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.SizingRule(ESizingRule::FixedSize);

	TSharedRef<SCortexCreateFilesDialog> Dialog =
		SNew(SCortexCreateFilesDialog)
		.Document(Document)
		.ParentWindow(DialogWindow);

	Dialog->DialogWindow = DialogWindow;

	DialogWindow->SetContent(Dialog);
	FSlateApplication::Get().AddModalWindow(DialogWindow, ParentWindow);

	return Dialog->bFilesCreated;
}

void SCortexCreateFilesDialog::Construct(const FArguments& InArgs)
{
	Document = InArgs._Document;

	// Snapshot code at open time
	if (Document.IsValid())
	{
		SnapshotHeader = Document->HeaderCode;
		SnapshotImpl = Document->ImplementationCode;
		ClassName = Document->ClassName;
	}

	// Default paths — strip UE type prefix (A/U/F...) from filename
	const FString SourceDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"));
	const FString FileName = StripUETypePrefix(ClassName);
	HeaderPath = FPaths::Combine(SourceDir, FString::Printf(TEXT("Public/%s.h"), *FileName));
	ImplPath = FPaths::Combine(SourceDir, FString::Printf(TEXT("Private/%s.cpp"), *FileName));

	ChildSlot
	[
		SNew(SBox)
		.Padding(FMargin(16))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(STextBlock).Text(NSLOCTEXT("CortexCreateFiles", "ClassLabel", "Class Name:"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SEditableTextBox)
				.Text(FText::FromString(ClassName))
				.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type)
				{
					ClassName = Text.ToString();
				})
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
			[
				SNew(STextBlock).Text(NSLOCTEXT("CortexCreateFiles", "HeaderLabel", "Header Path:"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SEditableTextBox)
				.Text(FText::FromString(HeaderPath))
				.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type)
				{
					HeaderPath = Text.ToString();
				})
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
			[
				SNew(STextBlock).Text(NSLOCTEXT("CortexCreateFiles", "ImplLabel", "Implementation Path:"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SEditableTextBox)
				.Text(FText::FromString(ImplPath))
				.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type)
				{
					ImplPath = Text.ToString();
				})
			]

			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNullWidget::NullWidget
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNullWidget::NullWidget
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
				[
					SNew(SButton)
					.Text(NSLOCTEXT("CortexCreateFiles", "Cancel", "Cancel"))
					.OnClicked(this, &SCortexCreateFilesDialog::OnCancelClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
				[
					SNew(SButton)
					.Text(NSLOCTEXT("CortexCreateFiles", "Create", "Create"))
					.OnClicked(this, &SCortexCreateFilesDialog::OnCreateClicked)
				]
			]
		]
	];
}

FReply SCortexCreateFilesDialog::OnCreateClicked()
{
	bool bHeaderOk = WriteFile(HeaderPath, SnapshotHeader);
	bool bImplOk = WriteFile(ImplPath, SnapshotImpl);

	bFilesCreated = bHeaderOk && bImplOk;

	if (DialogWindow.IsValid())
	{
		DialogWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

FReply SCortexCreateFilesDialog::OnCancelClicked()
{
	if (DialogWindow.IsValid())
	{
		DialogWindow->RequestDestroyWindow();
	}
	return FReply::Handled();
}

bool SCortexCreateFilesDialog::WriteFile(const FString& Path, const FString& Content)
{
	// Check for existing file
	if (IFileManager::Get().FileExists(*Path))
	{
		UE_LOG(LogCortexFrontend, Warning, TEXT("File already exists: %s — overwriting"), *Path);
	}

	if (!FFileHelper::SaveStringToFile(Content, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogCortexFrontend, Error, TEXT("Failed to write file: %s"), *Path);
		return false;
	}

	UE_LOG(LogCortexFrontend, Log, TEXT("Created file: %s"), *Path);
	return true;
}
