#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE(FOnCortexGenOverlayCancel);

class STextBlock;
class SProgressBar;
class SCircularThrobber;

class SCortexGenOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexGenOverlay) {}
		SLATE_EVENT(FOnCortexGenOverlayCancel, OnCancelClicked)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;

	void SetStatusText(const FString& Text);
	void SetQueuePosition(int32 Position);
	void SetProgress(float Fraction);
	void SetProgressIndeterminate(bool bIndeterminate);
	void SetExpectedTime(float Seconds);
	void ResetElapsed();
	void Show();
	void Hide();

	// Block all mouse input when visible
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override { return FReply::Handled(); }
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override { return FReply::Handled(); }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;

private:
	TSharedPtr<STextBlock> StatusLabel;
	TSharedPtr<STextBlock> QueueLabel;
	TSharedPtr<STextBlock> ElapsedLabel;
	TSharedPtr<STextBlock> ExpectedLabel;
	TSharedPtr<SProgressBar> ProgressBar;

	FOnCortexGenOverlayCancel OnCancelClicked;
	double StartTime = 0.0;
	bool bIsIndeterminate = true;
};
