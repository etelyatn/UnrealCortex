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
	const FSlateColor MutedColor = FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888"))));

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
		.Padding(4.0f, 4.0f)
		[
			SAssignNew(DetailLabel, STextBlock)
			.Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
			.ColorAndOpacity(MutedColor)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8.0f, 4.0f)
		[
			SAssignNew(ElapsedLabel, STextBlock)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			.ColorAndOpacity(MutedColor)
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

		StreamEventHandle = Session->OnStreamEvent.AddLambda([SelfWeak](const FCortexStreamEvent& Event)
		{
			if (TSharedPtr<SCortexProcessingIndicator> Self = SelfWeak.Pin())
			{
				Self->OnStreamEvent(Event);
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
		Session->OnStreamEvent.Remove(StreamEventHandle);
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
		CurrentToolName.Empty();
		ToolCallCount = 0;
	}
	UpdateVisibility(Change.NewState);
}

void SCortexProcessingIndicator::OnStreamEvent(const FCortexStreamEvent& Event)
{
	if (Event.Type == ECortexStreamEventType::ToolUse)
	{
		CurrentToolName = Event.ToolName;
		++ToolCallCount;
		UpdateStatusText();
	}
	else if (Event.Type == ECortexStreamEventType::ToolResult)
	{
		CurrentToolName.Empty();
		UpdateStatusText();
	}
	else if (Event.Type == ECortexStreamEventType::ContentBlockDelta
		|| Event.Type == ECortexStreamEventType::TextContent)
	{
		if (CurrentToolName.IsEmpty())
		{
			UpdateStatusText();
		}
	}
}

void SCortexProcessingIndicator::UpdateVisibility(ECortexSessionState State)
{
	CurrentState = State;
	if (State == ECortexSessionState::Spawning || State == ECortexSessionState::Processing)
	{
		SetVisibility(EVisibility::SelfHitTestInvisible);
		UpdateStatusText();
	}
	else
	{
		SetVisibility(EVisibility::Collapsed);
	}
}

void SCortexProcessingIndicator::UpdateStatusText()
{
	if (!StatusLabel.IsValid())
	{
		return;
	}

	if (CurrentState == ECortexSessionState::Spawning)
	{
		StatusLabel->SetText(FText::FromString(TEXT("Connecting...")));
		if (DetailLabel.IsValid())
		{
			DetailLabel->SetText(FText::GetEmpty());
		}
		return;
	}

	// During Processing, show what Claude is doing
	if (!CurrentToolName.IsEmpty())
	{
		StatusLabel->SetText(FText::FromString(TEXT("Using tool")));
		if (DetailLabel.IsValid())
		{
			DetailLabel->SetText(FText::FromString(CurrentToolName));
		}
	}
	else if (ToolCallCount > 0)
	{
		StatusLabel->SetText(FText::FromString(TEXT("Responding...")));
		if (DetailLabel.IsValid())
		{
			DetailLabel->SetText(FText::FromString(
				FString::Printf(TEXT("%d tool%s used"), ToolCallCount, ToolCallCount == 1 ? TEXT("") : TEXT("s"))));
		}
	}
	else
	{
		StatusLabel->SetText(FText::FromString(TEXT("Thinking...")));
		if (DetailLabel.IsValid())
		{
			DetailLabel->SetText(FText::GetEmpty());
		}
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
