#include "Widgets/SCortexGenOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Styling/AppStyle.h"
#include "HAL/PlatformTime.h"

void SCortexGenOverlay::Construct(const FArguments& InArgs)
{
	OnCancelClicked = InArgs._OnCancelClicked;
	SetCanTick(false);
	StartTime = FPlatformTime::Seconds();

	ChildSlot
	[
		SNew(SOverlay)

		// Dark semi-transparent background
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.ColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.6f))
		]

		// Content
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)

			// Throbber
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.f, 0.f, 0.f, 12.f)
			[
				SNew(SCircularThrobber)
				.Radius(16.f)
			]

			// Status text
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.f, 0.f, 0.f, 4.f)
			[
				SAssignNew(StatusLabel, STextBlock)
				.Text(FText::FromString(TEXT("Generating...")))
				.ColorAndOpacity(FLinearColor::White)
			]

			// Queue position
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.f, 0.f, 0.f, 8.f)
			[
				SAssignNew(QueueLabel, STextBlock)
				.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f))
				.Visibility(EVisibility::Collapsed)
			]

			// Progress bar
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(SBox)
				.WidthOverride(250.f)
				[
					SAssignNew(ProgressBar, SProgressBar)
				]
			]

			// Elapsed time
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.f, 0.f, 0.f, 4.f)
			[
				SAssignNew(ElapsedLabel, STextBlock)
				.Text(FText::FromString(TEXT("Elapsed: 0:00")))
				.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f))
			]

			// Expected time
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.f, 0.f, 0.f, 12.f)
			[
				SAssignNew(ExpectedLabel, STextBlock)
				.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f))
				.Visibility(EVisibility::Collapsed)
			]

			// Cancel button
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Cancel")))
				.OnClicked_Lambda([InArgs]()
				{
					InArgs._OnCancelClicked.ExecuteIfBound();
					return FReply::Handled();
				})
			]
		]
	];
}

void SCortexGenOverlay::Tick(const FGeometry& AllottedGeometry,
	const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (ElapsedLabel.IsValid())
	{
		double Elapsed = FPlatformTime::Seconds() - StartTime;
		int32 Minutes = static_cast<int32>(Elapsed) / 60;
		int32 Seconds = static_cast<int32>(Elapsed) % 60;
		ElapsedLabel->SetText(FText::FromString(
			FString::Printf(TEXT("Elapsed: %d:%02d"), Minutes, Seconds)));
	}
}

void SCortexGenOverlay::SetStatusText(const FString& Text)
{
	if (StatusLabel.IsValid())
	{
		StatusLabel->SetText(FText::FromString(Text));
	}
}

void SCortexGenOverlay::SetQueuePosition(int32 Position)
{
	if (QueueLabel.IsValid())
	{
		if (Position > 0)
		{
			QueueLabel->SetText(FText::FromString(
				FString::Printf(TEXT("Position #%d in queue"), Position)));
			QueueLabel->SetVisibility(EVisibility::Visible);
		}
		else
		{
			QueueLabel->SetVisibility(EVisibility::Collapsed);
		}
	}
}

void SCortexGenOverlay::SetProgress(float Fraction)
{
	if (ProgressBar.IsValid())
	{
		ProgressBar->SetPercent(Fraction);
	}
}

void SCortexGenOverlay::SetProgressIndeterminate(bool bIndeterminate)
{
	bIsIndeterminate = bIndeterminate;
	if (ProgressBar.IsValid())
	{
		if (bIndeterminate)
		{
			ProgressBar->SetPercent(TOptional<float>());
		}
	}
}

void SCortexGenOverlay::SetExpectedTime(float Seconds)
{
	if (ExpectedLabel.IsValid())
	{
		if (Seconds > 0.f)
		{
			int32 Secs = static_cast<int32>(Seconds);
			ExpectedLabel->SetText(FText::FromString(
				FString::Printf(TEXT("Usually takes ~%ds"), Secs)));
			ExpectedLabel->SetVisibility(EVisibility::Visible);
		}
		else
		{
			ExpectedLabel->SetVisibility(EVisibility::Collapsed);
		}
	}
}

void SCortexGenOverlay::ResetElapsed()
{
	StartTime = FPlatformTime::Seconds();
}

void SCortexGenOverlay::Show()
{
	SetVisibility(EVisibility::Visible);
	SetCanTick(true);
	ResetElapsed();
	FSlateApplication::Get().SetKeyboardFocus(SharedThis(this));
}

void SCortexGenOverlay::Hide()
{
	SetVisibility(EVisibility::Collapsed);
	SetCanTick(false);
}

FReply SCortexGenOverlay::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		OnCancelClicked.ExecuteIfBound();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}
