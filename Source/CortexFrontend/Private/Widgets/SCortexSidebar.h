#pragma once

#include "CoreMinimal.h"
#include "Session/CortexCliSession.h"
#include "Session/CortexSessionTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE(FOnCortexSidebarToggle);

class SCortexSidebar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexSidebar) {}
		SLATE_ARGUMENT(TWeakPtr<FCortexCliSession>, Session)
		SLATE_EVENT(FOnCortexSidebarToggle, OnCollapse)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	~SCortexSidebar();

	void SetCollapsed(bool bCollapsed);

private:
	void OnTokenUsageUpdated();
	void OnSessionStateChanged(const FCortexSessionStateChange& Change);
	void UpdateModelDisplay();

	TWeakPtr<FCortexCliSession> SessionWeak;
	FDelegateHandle TokenUsageHandle;
	FDelegateHandle StateChangedHandle;
	FOnCortexSidebarToggle OnCollapse;
	TSharedPtr<STextBlock> CollapseButtonText;
	TSharedPtr<STextBlock> ProviderText;
	TSharedPtr<STextBlock> ModelText;
	TSharedPtr<STextBlock> StateText;
};
