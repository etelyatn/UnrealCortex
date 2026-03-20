#include "Widgets/SCortexCodeCanvas.h"

#include "Widgets/SCortexCodeBlock.h"
#include "Widgets/SCortexConversionOverlay.h"
#include "Widgets/SCortexInheritedDiffView.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "HAL/PlatformApplicationMisc.h"

namespace
{
	const FLinearColor LabelColor(0.55f, 0.7f, 0.85f);
}

void SCortexCodeCanvas::Construct(const FArguments& InArgs)
{
	Document = InArgs._Document;
	ConversionContext = InArgs._ConversionContext;
	OnCreateFilesDelegate = InArgs._OnCreateFiles;

	const bool bInheritedMode = ConversionContext.IsValid()
		&& ConversionContext->SelectedDestination == ECortexConversionDestination::InjectIntoExisting;

	// ── Full-class layout: header (40%) + thin separator + implementation (60%) ──
	TSharedRef<SWidget> FullClassLayout = SNullWidget::NullWidget;

	if (bInheritedMode)
	{
		// Inherited mode: diff views showing changes against original files
		FullClassLayout =
			SNew(SVerticalBox)

			// .h (diff) label
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8, 4, 8, 2)
			[
				SAssignNew(HeaderLabel, STextBlock)
				.Text(NSLOCTEXT("CortexCodeCanvas", "HeaderDiffLabel", ".h (diff)"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(LabelColor))
			]

			// Header diff view (40% of remaining height)
			+ SVerticalBox::Slot()
			.FillHeight(0.4f)
			[
				SAssignNew(HeaderDiffView, SCortexInheritedDiffView)
				.OriginalText(ConversionContext->OriginalHeaderText)
				.CurrentText(FString())
			]

			// Thin divider
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SSeparator)
				.Orientation(Orient_Horizontal)
				.Thickness(1.0f)
			]

			// .cpp (diff) label
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8, 4, 8, 2)
			[
				SAssignNew(ImplLabel, STextBlock)
				.Text(NSLOCTEXT("CortexCodeCanvas", "ImplDiffLabel", ".cpp (diff)"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(LabelColor))
			]

			// Implementation diff view (60% of remaining height)
			+ SVerticalBox::Slot()
			.FillHeight(0.6f)
			[
				SAssignNew(ImplDiffView, SCortexInheritedDiffView)
				.OriginalText(ConversionContext->OriginalSourceText)
				.CurrentText(FString())
			];
	}
	else
	{
		// Standard mode: code blocks
		FullClassLayout =
			SNew(SVerticalBox)

			// .h label
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8, 4, 8, 2)
			[
				SAssignNew(HeaderLabel, STextBlock)
				.Text(NSLOCTEXT("CortexCodeCanvas", "HeaderLabel", ".h"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(LabelColor))
			]

			// Header code block (40% of remaining height)
			+ SVerticalBox::Slot()
			.FillHeight(0.4f)
			[
				SAssignNew(HeaderBlock, SCortexCodeBlock)
				.Code(Document.IsValid() ? Document->HeaderCode : FString())
				.Language(TEXT("cpp"))
			]

			// Thin divider
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SSeparator)
				.Orientation(Orient_Horizontal)
				.Thickness(1.0f)
			]

			// .cpp label
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8, 4, 8, 2)
			[
				SAssignNew(ImplLabel, STextBlock)
				.Text(NSLOCTEXT("CortexCodeCanvas", "ImplLabel", ".cpp"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(LabelColor))
			]

			// Implementation code block (60% of remaining height)
			+ SVerticalBox::Slot()
			.FillHeight(0.6f)
			[
				SAssignNew(ImplementationBlock, SCortexCodeBlock)
				.Code(Document.IsValid() ? Document->ImplementationCode : FString())
				.Language(TEXT("cpp"))
			];
	}

	// ── Snippet layout: single panel ──
	TSharedRef<SWidget> SnippetLayout =
		SNew(SVerticalBox)

		// Snippet label
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8, 4, 8, 2)
		[
			SAssignNew(SnippetLabel, STextBlock)
			.Text(NSLOCTEXT("CortexCodeCanvas", "SnippetLabel", "Snippet"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			.ColorAndOpacity(FSlateColor(LabelColor))
		]

		// Snippet code (fills remaining space)
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(SnippetBlock, SCortexCodeBlock)
			.Code(Document.IsValid() ? Document->SnippetCode : FString())
			.Language(TEXT("cpp"))
		];

	// ── Mode switcher: index 0 = full-class, index 1 = snippet ──
	// Start in full-class mode; switches to snippet when bIsSnippetMode is set
	ChildSlot
	[
		SNew(SVerticalBox)

		// Action bar: Copy + Create Files
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4)
		[
			SNew(SHorizontalBox)
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
				.Text(NSLOCTEXT("CortexCodeCanvas", "Copy", "Copy All"))
				.OnClicked(this, &SCortexCodeCanvas::OnCopyClicked)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2)
			[
				SNew(SButton)
				.Text_Lambda([this]() -> FText
				{
					const bool bInherited = ConversionContext.IsValid()
						&& ConversionContext->SelectedDestination == ECortexConversionDestination::InjectIntoExisting;
					return bInherited
						? NSLOCTEXT("CortexCodeCanvas", "Save", "Save")
						: NSLOCTEXT("CortexCodeCanvas", "CreateFiles", "Create Files");
				})
				.OnClicked(this, &SCortexCodeCanvas::OnCreateFilesButtonClicked)
			]
		]

		// Code display — wrapped in SOverlay so the processing animation can sit on top
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SOverlay)

			+ SOverlay::Slot()
			[
				SAssignNew(ModeSwitcher, SWidgetSwitcher)
				.WidgetIndex(0)

				// Index 0: full-class (header + implementation)
				+ SWidgetSwitcher::Slot()
				[
					FullClassLayout
				]

				// Index 1: snippet
				+ SWidgetSwitcher::Slot()
				[
					SnippetLayout
				]
			]

			// Animated overlay shown while LLM is generating (hidden by default)
			+ SOverlay::Slot()
			[
				SAssignNew(ProcessingOverlay, SCortexConversionOverlay)
			]
		]

		// Footer
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8, 4)
		[
			SNew(STextBlock)
			.Text_Lambda([this]() -> FText
			{
				const bool bInherited = ConversionContext.IsValid()
					&& ConversionContext->SelectedDestination == ECortexConversionDestination::InjectIntoExisting;
				return bInherited
					? NSLOCTEXT("CortexCodeCanvas", "InheritedFooter", "Inherited class mode \u2014 showing diff against original")
					: NSLOCTEXT("CortexCodeCanvas", "Footer", "Read-only \u2014 modify via chat");
			})
			.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
			.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
		]
	];

	// Processing overlay starts hidden; caller enables it via SetProcessing(true)
	if (ProcessingOverlay.IsValid())
	{
		ProcessingOverlay->SetVisibility(EVisibility::Collapsed);
	}

	// Subscribe to document changes
	if (Document.IsValid())
	{
		DocumentChangedHandle = Document->OnDocumentChanged.AddSP(
			this, &SCortexCodeCanvas::OnDocumentChanged);
	}

	// Set initial label state (shows "not generated" until code arrives)
	UpdateLabels();
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

	// Switch mode on first code arrival if snippet mode was set after construction
	if (ModeSwitcher.IsValid())
	{
		const int32 TargetIndex = Document->bIsSnippetMode ? 1 : 0;
		if (ModeSwitcher->GetActiveWidgetIndex() != TargetIndex)
		{
			ModeSwitcher->SetActiveWidgetIndex(TargetIndex);
		}
	}

	if (ChangedTab == ECortexCodeTab::Header && HeaderBlock.IsValid())
	{
		HeaderBlock->SetCode(Document->HeaderCode);
	}
	else if (ChangedTab == ECortexCodeTab::Implementation && ImplementationBlock.IsValid())
	{
		ImplementationBlock->SetCode(Document->ImplementationCode);
	}
	else if (ChangedTab == ECortexCodeTab::Snippet && SnippetBlock.IsValid())
	{
		SnippetBlock->SetCode(Document->SnippetCode);
	}

	// Route to diff views in inherited mode
	if (ChangedTab == ECortexCodeTab::Header && HeaderDiffView.IsValid())
	{
		HeaderDiffView->SetCurrentText(Document->HeaderCode);
	}
	else if (ChangedTab == ECortexCodeTab::Implementation && ImplDiffView.IsValid())
	{
		ImplDiffView->SetCurrentText(Document->ImplementationCode);
	}

	// Dismiss the processing overlay as soon as any code arrives
	if (bIsProcessing && (!Document->HeaderCode.IsEmpty() || !Document->ImplementationCode.IsEmpty() || !Document->SnippetCode.IsEmpty()))
	{
		SetProcessing(false);
	}

	UpdateLabels();
}

void SCortexCodeCanvas::SetProcessing(bool bProcessing)
{
	bIsProcessing = bProcessing;
	if (ProcessingOverlay.IsValid())
	{
		if (bProcessing)
		{
			ProcessingOverlay->ResetTimer();
		}
		ProcessingOverlay->SetVisibility(bProcessing ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed);
	}
}

void SCortexCodeCanvas::SetTokenCount(int32 Tokens)
{
	if (ProcessingOverlay.IsValid())
	{
		ProcessingOverlay->SetTokenCount(Tokens);
	}
}

FReply SCortexCodeCanvas::OnCopyClicked()
{
	if (!Document.IsValid())
	{
		return FReply::Handled();
	}

	if (Document->bIsSnippetMode)
	{
		FPlatformApplicationMisc::ClipboardCopy(*Document->SnippetCode);
	}
	else
	{
		// Copy both header and implementation separated by a marker
		FString Combined = FString::Printf(TEXT("// === %s.h ===\n%s\n\n// === %s.cpp ===\n%s"),
			*Document->ClassName, *Document->HeaderCode,
			*Document->ClassName, *Document->ImplementationCode);
		FPlatformApplicationMisc::ClipboardCopy(*Combined);
	}

	return FReply::Handled();
}

FReply SCortexCodeCanvas::OnCreateFilesButtonClicked()
{
	OnCreateFilesDelegate.ExecuteIfBound();
	return FReply::Handled();
}

void SCortexCodeCanvas::UpdateLabels()
{
	if (!Document.IsValid())
	{
		return;
	}

	if (Document->bIsSnippetMode && SnippetLabel.IsValid())
	{
		const int32 Lines = CountLines(Document->SnippetCode);
		FString Label = Lines > 0
			? FString::Printf(TEXT("Snippet (%d lines)"), Lines)
			: TEXT("Snippet");
		SnippetLabel->SetText(FText::FromString(Label));
		return;
	}

	if (HeaderLabel.IsValid())
	{
		const int32 Lines = CountLines(Document->HeaderCode);
		FString Label = Lines > 0
			? FString::Printf(TEXT("%s.h (%d lines)"), *Document->ClassName, Lines)
			: FString::Printf(TEXT("%s.h \u2014 not generated"), *Document->ClassName);
		HeaderLabel->SetText(FText::FromString(Label));
	}

	if (ImplLabel.IsValid())
	{
		const int32 Lines = CountLines(Document->ImplementationCode);
		FString Label = Lines > 0
			? FString::Printf(TEXT("%s.cpp (%d lines)"), *Document->ClassName, Lines)
			: FString::Printf(TEXT("%s.cpp \u2014 not generated"), *Document->ClassName);
		ImplLabel->SetText(FText::FromString(Label));
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
