#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SWidget;
class STextBlock;

/**
 * Game-view overlay of an owned replay run: a centred hands-off countdown while the run prepares and
 * a top banner while it plays. It is display only: the owner constructs it with
 * EVisibility::HitTestInvisible, it never supports focus and nothing in it handles input.
 */
class SCortexReplayRunOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexReplayRunOverlay) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Shows the centred countdown block (and hides the banner) with the whole seconds left. */
	void ShowCountdown(int32 SecondsLeft);

	/** Shows the top banner (and hides the countdown block) for the named recording. */
	void ShowPlaying(const FString& RecordingName);

	/** The sentence the overlay currently shows: the countdown request or the banner text. */
	FText GetHeadline() const;

private:
	bool bShowingCountdown = false;
	int32 ShownSeconds = INDEX_NONE;
	FText Headline;
	TSharedPtr<SWidget> CountdownBlock;
	TSharedPtr<SWidget> BannerBlock;
	TSharedPtr<STextBlock> CountdownNumber;
	TSharedPtr<STextBlock> CountdownText;
	TSharedPtr<STextBlock> BannerText;
};
