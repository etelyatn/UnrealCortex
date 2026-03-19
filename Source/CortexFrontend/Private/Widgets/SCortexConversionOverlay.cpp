#include "Widgets/SCortexConversionOverlay.h"

#include "Utilities/CortexTokenUtils.h"
#include "HAL/PlatformTime.h"
#include "Layout/Geometry.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace CortexConversionOverlayColors
{
	// Background matches the code block dark surface
	const FLinearColor BgColor = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("0f0f0f")));
	// Title: comment green from the syntax highlighter (#6a9955), Mono font reads as a code comment
	const FLinearColor Title = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("6a9955")));
	// Dots: amber — same as SCortexProcessingIndicator for visual consistency
	const FLinearColor Dots = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("f59e0b")));
	// Phase / elapsed text: matches the muted gray used across indicators
	const FLinearColor Muted = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888")));
	// Scan line: very subtle warm wash
	const FLinearColor ScanLine(0.25f, 0.18f, 0.06f, 0.07f);
}

void SCortexConversionOverlay::Construct(const FArguments& InArgs)
{
	StartTime   = FPlatformTime::Seconds();
	LastDotTime = StartTime;
	CustomPhaseLabels = InArgs._PhaseLabels;
	SetCanTick(true);

	ChildSlot
	.HAlign(HAlign_Center)
	.VAlign(VAlign_Center)
	[
		SNew(SBox)
		.WidthOverride(300.0f)
		[
			SNew(SVerticalBox)

			// Title
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				SNew(STextBlock)
				.Text(InArgs._Title)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 13))
				.ColorAndOpacity(FSlateColor(CortexConversionOverlayColors::Title))
			]

			// Phase label
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.0f, 0.0f, 0.0f, 18.0f)
			[
				SAssignNew(PhaseLabel, STextBlock)
				.Text(NSLOCTEXT("CortexConversionOverlay", "Phase", "Serializing Blueprint..."))
				.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
				.ColorAndOpacity(FSlateColor(CortexConversionOverlayColors::Muted))
			]

			// Animated dots
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				SNew(SBox)
				.WidthOverride(56.0f)
				.HAlign(HAlign_Center)
				[
					SAssignNew(DotsLabel, STextBlock)
					.Text(FText::FromString(TEXT("\u25CF")))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
					.ColorAndOpacity(FSlateColor(CortexConversionOverlayColors::Dots))
				]
			]

			// Token count + ETA (populated once serialization completes)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SAssignNew(TokenLabel, STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(CortexConversionOverlayColors::Muted))
				.Visibility(EVisibility::Collapsed)
			]

			// Elapsed / estimated time
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SAssignNew(ElapsedLabel, STextBlock)
				.Text(FText::FromString(TEXT("0s")))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(CortexConversionOverlayColors::Muted))
			]
		]
	];
}

void SCortexConversionOverlay::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	AnimTime += InDeltaTime;

	// Animate dots at 400 ms per phase (3 phases)
	const double Now = FPlatformTime::Seconds();
	if (Now - LastDotTime >= 0.4)
	{
		DotPhase    = (DotPhase + 1) % 3;
		LastDotTime = Now;

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
	}

	// Elapsed / ETA counter
	if (ElapsedLabel.IsValid())
	{
		const int32 Elapsed = static_cast<int32>(Now - StartTime);

		if (EstimatedSeconds > 0.0f)
		{
			const int32 Remaining = FMath::Max(0, static_cast<int32>(EstimatedSeconds) - Elapsed);
			FString Label;
			if (Elapsed > static_cast<int32>(EstimatedSeconds) + 10)
			{
				Label = FString::Printf(TEXT("%ds (running long...)"), Elapsed);
			}
			else if (Remaining <= 5)
			{
				Label = TEXT("any moment now...");
			}
			else
			{
				Label = FString::Printf(TEXT("%ds / ~%ds"), Elapsed, static_cast<int32>(EstimatedSeconds));
			}
			ElapsedLabel->SetText(FText::FromString(Label));
		}
		else
		{
			ElapsedLabel->SetText(FText::FromString(FString::Printf(TEXT("%ds"), Elapsed)));
		}
	}

	// Phase label cycles based on elapsed time
	if (PhaseLabel.IsValid())
	{
		const double Elapsed = Now - StartTime;
		FString Phase;
		if (CustomPhaseLabels.Num() >= 4)
		{
			if      (Elapsed < 3.0)  Phase = CustomPhaseLabels[0];
			else if (Elapsed < 6.0)  Phase = CustomPhaseLabels[1];
			else if (Elapsed < 12.0) Phase = CustomPhaseLabels[2];
			else                     Phase = CustomPhaseLabels[3];
		}
		else
		{
			if      (Elapsed < 3.0)  Phase = TEXT("Serializing Blueprint...");
			else if (Elapsed < 6.0)  Phase = TEXT("Starting Claude session...");
			else if (Elapsed < 12.0) Phase = TEXT("Sending to LLM...");
			else                     Phase = TEXT("Generating C++ code...");
		}
		PhaseLabel->SetText(FText::FromString(Phase));
	}

	// Invalidate so the scan-line drawn in OnPaint is refreshed every frame
	Invalidate(EInvalidateWidgetReason::Paint);
}

int32 SCortexConversionOverlay::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FSlateBrush* WhiteBrush = FAppStyle::GetBrush(TEXT("WhiteBrush"));
	const FVector2f Size = AllottedGeometry.GetLocalSize();

	// Dark semi-transparent background
	FSlateDrawElement::MakeBox(
		OutDrawElements, LayerId,
		AllottedGeometry.ToPaintGeometry(),
		WhiteBrush,
		ESlateDrawEffect::None,
		CortexConversionOverlayColors::BgColor);

	// Scan line — a soft horizontal band that scrolls top-to-bottom
	const float BandHeight = 80.0f;
	const float CycleLength = Size.Y + BandHeight;
	const float ScanY = FMath::Fmod(AnimTime * 55.0f, CycleLength) - BandHeight;

	FSlateDrawElement::MakeBox(
		OutDrawElements, LayerId + 1,
		AllottedGeometry.ToPaintGeometry(
			FVector2f(Size.X, BandHeight),
			FSlateLayoutTransform(FVector2f(0.0f, ScanY))),
		WhiteBrush,
		ESlateDrawEffect::None,
		CortexConversionOverlayColors::ScanLine);

	// Child widgets (centered text content) rendered on top
	return SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect,
		OutDrawElements, LayerId + 2, InWidgetStyle, bParentEnabled);
}

void SCortexConversionOverlay::ResetTimer()
{
	StartTime = FPlatformTime::Seconds();
	LastDotTime = StartTime;
	AnimTime = 0.0f;
	DotPhase = 0;
	TokenCount = 0;
	EstimatedSeconds = 0.0f;

	if (ElapsedLabel.IsValid())
	{
		ElapsedLabel->SetText(FText::FromString(TEXT("0s")));
	}
	if (PhaseLabel.IsValid())
	{
		const FText InitialPhase = (CustomPhaseLabels.Num() >= 4)
			? FText::FromString(CustomPhaseLabels[0])
			: NSLOCTEXT("CortexConversionOverlay", "Phase", "Serializing Blueprint...");
		PhaseLabel->SetText(InitialPhase);
	}
	if (TokenLabel.IsValid())
	{
		TokenLabel->SetVisibility(EVisibility::Collapsed);
	}
}

void SCortexConversionOverlay::SetTokenCount(int32 Tokens)
{
	TokenCount = Tokens;
	EstimatedSeconds = CortexTokenUtils::EstimateSecondsForTokens(Tokens);

	if (TokenLabel.IsValid())
	{
		const FString Label = CortexTokenUtils::FormatTokenEstimate(Tokens);
		TokenLabel->SetText(FText::FromString(Label));
		TokenLabel->SetVisibility(Label.IsEmpty() ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible);
	}
}
