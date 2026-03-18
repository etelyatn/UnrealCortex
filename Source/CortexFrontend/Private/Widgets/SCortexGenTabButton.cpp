#include "Widgets/SCortexGenTabButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

void SCortexGenTabButton::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "NoBorder")
		.OnClicked(InArgs._OnClicked)
		.ContentPadding(FMargin(8.f, 4.f))
		[
			SNew(SHorizontalBox)

			// Spinning throbber — visible only while Generating
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 4.f, 0.f)
			[
				SAssignNew(Throbber, SCircularThrobber)
				.Radius(6.f)
				.Visibility(EVisibility::Collapsed)
			]

			// Status icon — visible for Complete / Error
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 4.f, 0.f)
			[
				SAssignNew(StatusIcon, SImage)
				.Visibility(EVisibility::Collapsed)
			]

			// Tab display name
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SAssignNew(NameLabel, STextBlock)
				.Text(InArgs._DisplayName)
			]

			// Close button
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "NoBorder")
				.ContentPadding(FMargin(2.f))
				.OnClicked_Lambda([InArgs]()
				{
					InArgs._OnCloseClicked.ExecuteIfBound();
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("x")))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				]
			]
		]
	];
}

void SCortexGenTabButton::SetStatus(ECortexGenSessionStatus NewStatus)
{
	CurrentStatus = NewStatus;

	if (!Throbber.IsValid() || !StatusIcon.IsValid())
	{
		return;
	}

	switch (NewStatus)
	{
	case ECortexGenSessionStatus::Generating:
		Throbber->SetVisibility(EVisibility::Visible);
		StatusIcon->SetVisibility(EVisibility::Collapsed);
		break;

	case ECortexGenSessionStatus::Complete:
	case ECortexGenSessionStatus::PartialComplete:
		Throbber->SetVisibility(EVisibility::Collapsed);
		StatusIcon->SetImage(FAppStyle::GetBrush("Symbols.Check"));
		StatusIcon->SetVisibility(EVisibility::Visible);
		break;

	case ECortexGenSessionStatus::Error:
		Throbber->SetVisibility(EVisibility::Collapsed);
		StatusIcon->SetImage(FAppStyle::GetBrush("Icons.Error.Solid"));
		StatusIcon->SetVisibility(EVisibility::Visible);
		break;

	default: // Idle
		Throbber->SetVisibility(EVisibility::Collapsed);
		StatusIcon->SetVisibility(EVisibility::Collapsed);
		break;
	}
}

void SCortexGenTabButton::SetDisplayName(const FText& Name)
{
	if (NameLabel.IsValid())
	{
		NameLabel->SetText(Name);
	}
}
