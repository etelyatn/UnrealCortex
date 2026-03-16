#include "Widgets/SCortexCodeCanvas.h"

#include "Widgets/SCortexCodeBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "HAL/PlatformApplicationMisc.h"

void SCortexCodeCanvas::Construct(const FArguments& InArgs)
{
	Document = InArgs._Document;
	OnCreateFilesDelegate = InArgs._OnCreateFiles;

	ChildSlot
	[
		SNew(SVerticalBox)

		// Tab bar: .h / .cpp + Copy + Create Files
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2)
			[
				SNew(SButton)
				.Text(NSLOCTEXT("CortexCodeCanvas", "HeaderTab", ".h"))
				.OnClicked_Lambda([this]()
				{
					SwitchToTab(ECortexCodeTab::Header);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2)
			[
				SNew(SButton)
				.Text(NSLOCTEXT("CortexCodeCanvas", "ImplTab", ".cpp"))
				.OnClicked_Lambda([this]()
				{
					SwitchToTab(ECortexCodeTab::Implementation);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNullWidget::NullWidget
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2)
			[
				SNew(SButton)
				.Text(NSLOCTEXT("CortexCodeCanvas", "Copy", "Copy"))
				.OnClicked(this, &SCortexCodeCanvas::OnCopyClicked)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2)
			[
				SNew(SButton)
				.Text(NSLOCTEXT("CortexCodeCanvas", "CreateFiles", "Create Files"))
				.OnClicked(this, &SCortexCodeCanvas::OnCreateFilesButtonClicked)
			]
		]

		// Code display
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(CodeSwitcher, SWidgetSwitcher)
			.WidgetIndex(0)
			+ SWidgetSwitcher::Slot()
			[
				SAssignNew(HeaderBlock, SCortexCodeBlock)
				.Code(Document.IsValid() ? Document->HeaderCode : FString())
				.Language(TEXT("cpp"))
			]
			+ SWidgetSwitcher::Slot()
			[
				SAssignNew(ImplementationBlock, SCortexCodeBlock)
				.Code(Document.IsValid() ? Document->ImplementationCode : FString())
				.Language(TEXT("cpp"))
			]
		]

		// Footer
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8, 4)
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexCodeCanvas", "Footer", "Read-only — modify via chat"))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
			.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
		]
	];

	// Subscribe to document changes
	if (Document.IsValid())
	{
		DocumentChangedHandle = Document->OnDocumentChanged.AddSP(
			this, &SCortexCodeCanvas::OnDocumentChanged);
	}
}

SCortexCodeCanvas::~SCortexCodeCanvas()
{
	if (Document.IsValid())
	{
		Document->OnDocumentChanged.Remove(DocumentChangedHandle);
	}
}

void SCortexCodeCanvas::OnDocumentChanged(ECortexCodeTab ChangedTab)
{
	if (!Document.IsValid())
	{
		return;
	}

	if (ChangedTab == ECortexCodeTab::Header && HeaderBlock.IsValid())
	{
		HeaderBlock->SetCode(Document->HeaderCode);
	}
	else if (ChangedTab == ECortexCodeTab::Implementation && ImplementationBlock.IsValid())
	{
		ImplementationBlock->SetCode(Document->ImplementationCode);
	}
}

FReply SCortexCodeCanvas::OnCopyClicked()
{
	if (!Document.IsValid())
	{
		return FReply::Handled();
	}

	const FString& Code = (CurrentTab == ECortexCodeTab::Header)
		? Document->HeaderCode
		: Document->ImplementationCode;

	FPlatformApplicationMisc::ClipboardCopy(*Code);
	return FReply::Handled();
}

FReply SCortexCodeCanvas::OnCreateFilesButtonClicked()
{
	OnCreateFilesDelegate.ExecuteIfBound();
	return FReply::Handled();
}

void SCortexCodeCanvas::SwitchToTab(ECortexCodeTab Tab)
{
	CurrentTab = Tab;
	if (CodeSwitcher.IsValid())
	{
		CodeSwitcher->SetActiveWidgetIndex(Tab == ECortexCodeTab::Header ? 0 : 1);
	}
}
