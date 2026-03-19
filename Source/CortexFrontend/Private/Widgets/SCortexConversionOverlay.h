#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;

/**
 * Animated full-canvas overlay shown while the LLM is processing.
 * Draws a dark background with a moving scan line and animated status text.
 * Hide by setting visibility to Collapsed once results arrive.
 * Reusable across conversion and analysis flows via Title/PhaseLabels args.
 */
class SCortexConversionOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexConversionOverlay)
		: _Title(NSLOCTEXT("CortexConversionOverlay", "Title", "// Generating C++"))
	{}
		SLATE_ARGUMENT(FText, Title)
		/** Optional phase labels [phase0, phase1, phase2, phase3] — elapsed-time based cycling. */
		SLATE_ARGUMENT(TArray<FString>, PhaseLabels)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	/** Provide the estimated token count so the overlay can show an ETA.
	 *  Total estimate = ~10s connection overhead + (tokens / 1000) * 10s LLM execution. */
	void SetTokenCount(int32 Tokens);

	/** Reset the elapsed timer and animation state. Call when processing starts. */
	void ResetTimer();

private:
	TSharedPtr<STextBlock> DotsLabel;
	TSharedPtr<STextBlock> ElapsedLabel;
	TSharedPtr<STextBlock> PhaseLabel;
	TSharedPtr<STextBlock> TokenLabel;

	double StartTime = 0.0;
	int32 DotPhase = 0;
	double LastDotTime = 0.0;
	float AnimTime = 0.0f;

	int32 TokenCount = 0;
	float EstimatedSeconds = 0.0f;

	TArray<FString> CustomPhaseLabels;
};
