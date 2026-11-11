#include "Widgets/SCortexReplayRunOverlay.h"

#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CortexReplayRunOverlay"

void SCortexReplayRunOverlay::Construct(const FArguments& InArgs)
{
	(void)InArgs;
	const FSlateBrush* Backdrop = FAppStyle::Get().GetBrush("WhiteBrush");
	const FLinearColor Shade(0.0f, 0.0f, 0.0f, 0.65f);

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SAssignNew(CountdownBlock, SBorder)
			.Visibility(EVisibility::Collapsed)
			.BorderImage(Backdrop)
			.BorderBackgroundColor(Shade)
			.Padding(FMargin(32.0f, 20.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SAssignNew(CountdownNumber, STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 72))
					.ColorAndOpacity(FLinearColor::White)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
				[
					SAssignNew(CountdownText, STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 20))
					.ColorAndOpacity(FLinearColor::White)
				]
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Top)
		[
			SAssignNew(BannerBlock, SBorder)
			.Visibility(EVisibility::Collapsed)
			.BorderImage(Backdrop)
			.BorderBackgroundColor(Shade)
			.Padding(FMargin(16.0f, 8.0f))
			[
				SAssignNew(BannerText, STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
				.ColorAndOpacity(FLinearColor::White)
				.Justification(ETextJustify::Center)
			]
		]
	];
}

void SCortexReplayRunOverlay::ShowCountdown(int32 SecondsLeft)
{
	if (bShowingCountdown && SecondsLeft == ShownSeconds)
	{
		return;
	}
	bShowingCountdown = true;
	ShownSeconds = SecondsLeft;
	Headline = LOCTEXT("CountdownHeadline", "Replay starting \u2014 don't touch the mouse or keyboard");
	CountdownNumber->SetText(FText::AsNumber(FMath::Max(SecondsLeft, 0)));
	CountdownText->SetText(Headline);
	BannerBlock->SetVisibility(EVisibility::Collapsed);
	CountdownBlock->SetVisibility(EVisibility::SelfHitTestInvisible);
}

void SCortexReplayRunOverlay::ShowPlaying(const FString& RecordingName)
{
	bShowingCountdown = false;
	Headline = FText::Format(
		LOCTEXT("PlayingHeadline", "Replaying '{0}' \u2014 touching the mouse or keyboard stops it"),
		FText::FromString(RecordingName));
	BannerText->SetText(Headline);
	CountdownBlock->SetVisibility(EVisibility::Collapsed);
	BannerBlock->SetVisibility(EVisibility::SelfHitTestInvisible);
}

FText SCortexReplayRunOverlay::GetHeadline() const
{
	return Headline;
}

#undef LOCTEXT_NAMESPACE
