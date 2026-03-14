#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SCortexSidebar.h"

class FCortexCliSession;

class SCortexWorkbench : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexWorkbench) {}
		SLATE_ARGUMENT(TSharedPtr<SDockTab>, OwnerTab)
		SLATE_ARGUMENT(TWeakPtr<FCortexCliSession>, Session)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCortexWorkbench();

	TSharedPtr<FTabManager> GetTabManager() const { return TabManager; }

private:
	TSharedRef<SDockTab> SpawnChatTab(const FSpawnTabArgs& Args);
	void OnSidebarToggle();
	float GetSidebarSlotValue() const;

	TSharedPtr<FTabManager> TabManager;
	TWeakPtr<FCortexCliSession> SessionWeak;
	TSharedPtr<SCortexSidebar> Sidebar;
	TSharedPtr<SBox> SidebarBox;

	float CachedSidebarCoefficient = 0.20f;
	bool bSidebarCollapsed = false;
};
