#include "Widgets/SCortexCodeCanvas.h"

#include "Widgets/SCortexCodeBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "HAL/PlatformApplicationMisc.h"

namespace
{
	const FLinearColor ActiveTabColor(0.9f, 0.9f, 0.9f);
	const FLinearColor InactiveTabColor(0.45f, 0.45f, 0.45f);
}

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
				.OnClicked_Lambda([this]()
				{
					SwitchToTab(ECortexCodeTab::Header);
					return FReply::Handled();
				})
				[
					SAssignNew(HeaderTabLabel, STextBlock)
					.Text(NSLOCTEXT("CortexCodeCanvas", "HeaderTab", ".h"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FSlateColor(ActiveTabColor))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2)
			[
				SNew(SButton)
				.OnClicked_Lambda([this]()
				{
					SwitchToTab(ECortexCodeTab::Implementation);
					return FReply::Handled();
				})
				[
					SAssignNew(ImplTabLabel, STextBlock)
					.Text(NSLOCTEXT("CortexCodeCanvas", "ImplTab", ".cpp"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
					.ColorAndOpacity(FSlateColor(InactiveTabColor))
				]
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

	UpdateTabLabels();
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
	UpdateTabLabels();
}

void SCortexCodeCanvas::UpdateTabLabels()
{
	if (!Document.IsValid())
	{
		return;
	}

	const int32 HeaderLines = CountLines(Document->HeaderCode);
	const int32 ImplLines = CountLines(Document->ImplementationCode);
	const bool bHeaderActive = (CurrentTab == ECortexCodeTab::Header);

	if (HeaderTabLabel.IsValid())
	{
		FString Label = HeaderLines > 0
			? FString::Printf(TEXT(".h (%d lines)"), HeaderLines)
			: TEXT(".h");
		HeaderTabLabel->SetText(FText::FromString(Label));
		HeaderTabLabel->SetFont(FCoreStyle::GetDefaultFontStyle(bHeaderActive ? "Bold" : "Regular", 10));
		HeaderTabLabel->SetColorAndOpacity(FSlateColor(bHeaderActive ? ActiveTabColor : InactiveTabColor));
	}

	if (ImplTabLabel.IsValid())
	{
		FString Label = ImplLines > 0
			? FString::Printf(TEXT(".cpp (%d lines)"), ImplLines)
			: TEXT(".cpp");
		ImplTabLabel->SetText(FText::FromString(Label));
		ImplTabLabel->SetFont(FCoreStyle::GetDefaultFontStyle(!bHeaderActive ? "Bold" : "Regular", 10));
		ImplTabLabel->SetColorAndOpacity(FSlateColor(!bHeaderActive ? ActiveTabColor : InactiveTabColor));
	}
}

int32 SCortexCodeCanvas::CountLines(const FString& Code)
{
	if (Code.IsEmpty())
	{
		return 0;
	}

	int32 Lines = 1;
	for (const TCHAR& Ch : Code)
	{
		if (Ch == TEXT('\n'))
		{
			++Lines;
		}
	}
	return Lines;
}
