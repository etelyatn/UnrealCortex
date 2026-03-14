#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FCortexCliSession;
class SCortexSidebar;

class SCortexWorkbench : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexWorkbench) {}
		SLATE_ARGUMENT(TSharedPtr<SDockTab>, OwnerTab)
		SLATE_ARGUMENT(TWeakPtr<FCortexCliSession>, Session)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	TSharedPtr<FTabManager> GetTabManager() const { return TabManager; }

private:
	TSharedRef<SDockTab> SpawnChatTab(const FSpawnTabArgs& Args);

	TSharedPtr<FTabManager> TabManager;
	TWeakPtr<FCortexCliSession> SessionWeak;
	TSharedPtr<SCortexSidebar> Sidebar;

	float CachedSidebarCoefficient = 0.20f;
};
