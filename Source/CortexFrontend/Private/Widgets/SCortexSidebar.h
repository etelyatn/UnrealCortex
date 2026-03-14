#pragma once

#include "CoreMinimal.h"
#include "Session/CortexCliSession.h"
#include "Session/CortexSessionTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexSidebar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexSidebar) {}
		SLATE_ARGUMENT(TWeakPtr<FCortexCliSession>, Session)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetCollapsed(bool bCollapsed);

private:
	void OnTokenUsageUpdated();
	void OnSessionStateChanged(const FCortexSessionStateChange& Change);
	void UpdateTokenDisplay();
	void UpdateModelDisplay();

	TWeakPtr<FCortexCliSession> SessionWeak;
	TSharedPtr<STextBlock> ProviderText;
	TSharedPtr<STextBlock> ModelText;
	TSharedPtr<STextBlock> InputTokensText;
	TSharedPtr<STextBlock> OutputTokensText;
	TSharedPtr<STextBlock> CacheTokensText;
	TSharedPtr<STextBlock> CacheHitRateText;
	TSharedPtr<STextBlock> StateText;
};
