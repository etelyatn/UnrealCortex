#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FCortexCliSession;
class SBox;

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

	/** Spawn a new chat tab with a fresh session. */
	void SpawnNewChatTab();

private:
	void SwitchToMultiTabMode();
	TSharedRef<SDockTab> SpawnChatTab(const FSpawnTabArgs& Args);
	void CleanupChatTab(FName TabId);
	TSharedRef<SDockTab> BuildChatTab(TSharedPtr<FCortexCliSession> Session, const FString& Label, FName TabId);

	TSharedPtr<FTabManager> TabManager;
	TWeakPtr<FCortexCliSession> SessionWeak;
	TWeakPtr<SDockTab> OwnerTabWeak;
	TSharedPtr<SBox> ContentContainer;

	TMap<FName, TSharedPtr<FCortexCliSession>> ChatSessions;
	int32 ChatTabCounter = 0;
};
