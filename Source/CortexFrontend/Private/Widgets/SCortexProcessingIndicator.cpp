#include "Widgets/SCortexProcessingIndicator.h"

#include "HAL/PlatformTime.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SCortexProcessingIndicator::Construct(const FArguments& InArgs)
{
	SessionWeak = InArgs._Session;
	SetCanTick(true);

	const FSlateColor AmberColor = FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("f59e0b"))));

	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8.0f, 4.0f)
		[
			SAssignNew(StatusLabel, STextBlock)
			.Text(FText::FromString(TEXT("Connecting...")))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.ColorAndOpacity(AmberColor)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4.0f, 4.0f)
		[
			SNew(SBox)
			.WidthOverride(48.0f)
			[
				SAssignNew(DotsLabel, STextBlock)
				.Text(FText::FromString(TEXT("\u25CF")))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(AmberColor)
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8.0f, 4.0f)
		[
			SAssignNew(ElapsedLabel, STextBlock)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			.ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888")))))
		]
	];

	if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
	{
		TWeakPtr<SCortexProcessingIndicator> SelfWeak = SharedThis(this);
		StateChangedHandle = Session->OnStateChanged.AddLambda([SelfWeak](const FCortexSessionStateChange& Change)
		{
			if (TSharedPtr<SCortexProcessingIndicator> Self = SelfWeak.Pin())
			{
				Self->OnSessionStateChanged(Change);
			}
		});

		UpdateVisibility(Session->GetState());
	}
	else
	{
		SetVisibility(EVisibility::Collapsed);
	}
}

SCortexProcessingIndicator::~SCortexProcessingIndicator()
{
	if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
	{
		Session->OnStateChanged.Remove(StateChangedHandle);
	}
}

void SCortexProcessingIndicator::OnSessionStateChanged(const FCortexSessionStateChange& Change)
{
	CurrentState = Change.NewState;
	if (Change.NewState == ECortexSessionState::Spawning || Change.NewState == ECortexSessionState::Processing)
	{
		IndicatorStartTime = FPlatformTime::Seconds();
		DotPhase = 0;
		LastDotTime = IndicatorStartTime;
	}
	UpdateVisibility(Change.NewState);
}

void SCortexProcessingIndicator::UpdateVisibility(ECortexSessionState State)
{
	CurrentState = State;
	if (State == ECortexSessionState::Spawning || State == ECortexSessionState::Processing)
	{
		SetVisibility(EVisibility::SelfHitTestInvisible);

		if (StatusLabel.IsValid())
		{
			StatusLabel->SetText(FText::FromString(
				State == ECortexSessionState::Spawning ? TEXT("Connecting...") : TEXT("Processing...")));
		}
	}
	else
	{
		SetVisibility(EVisibility::Collapsed);
	}
}

void SCortexProcessingIndicator::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (GetVisibility() == EVisibility::Collapsed)
	{
		return;
	}

	// Animate dots at 400ms per phase (3 phases)
	const double Now = FPlatformTime::Seconds();
	if (Now - LastDotTime >= 0.4)
	{
		DotPhase = (DotPhase + 1) % 3;
		LastDotTime = Now;
	}

	if (DotsLabel.IsValid())
	{
		FString Dots;
		for (int32 i = 0; i <= DotPhase; ++i)
		{
			if (i > 0) Dots += TEXT("  ");
			Dots += TEXT("\u25CF");
		}
		DotsLabel->SetText(FText::FromString(Dots));
	}

	// Elapsed time
	if (ElapsedLabel.IsValid() && IndicatorStartTime > 0.0)
	{
		const int32 Elapsed = static_cast<int32>(Now - IndicatorStartTime);
		ElapsedLabel->SetText(FText::FromString(FString::Printf(TEXT("%ds"), Elapsed)));
	}

}
