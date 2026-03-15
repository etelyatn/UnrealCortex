#pragma once

#include "CoreMinimal.h"
#include "Session/CortexCliSession.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexProcessingIndicator : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexProcessingIndicator) {}
		SLATE_ARGUMENT(TWeakPtr<FCortexCliSession>, Session)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	~SCortexProcessingIndicator();
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	void OnSessionStateChanged(const FCortexSessionStateChange& Change);
	void OnStreamEvent(const FCortexStreamEvent& Event);
	void UpdateVisibility(ECortexSessionState State);
	void UpdateStatusText();

	TWeakPtr<FCortexCliSession> SessionWeak;
	FDelegateHandle StateChangedHandle;
	FDelegateHandle StreamEventHandle;
	TSharedPtr<STextBlock> StatusLabel;
	TSharedPtr<STextBlock> DotsLabel;
	TSharedPtr<STextBlock> ElapsedLabel;
	TSharedPtr<STextBlock> DetailLabel;
	double IndicatorStartTime = 0.0;
	int32 DotPhase = 0;
	double LastDotTime = 0.0;
	ECortexSessionState CurrentState = ECortexSessionState::Inactive;
	FString CurrentToolName;
	int32 ToolCallCount = 0;
};
