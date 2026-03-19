#pragma once

#include "CoreMinimal.h"
#include "Analysis/CortexAnalysisContext.h"
#include "Conversion/CortexConversionContext.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SCortexSidebar.h"

class FCortexCliSession;
struct FCortexAnalysisPayload;
struct FCortexConversionPayload;

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

	/** Spawn a new conversion tab for the given payload. */
	void SpawnConversionTab(const FCortexConversionPayload& Payload);

	/** Spawn a new analysis tab for the given payload. */
	void SpawnAnalysisTab(const FCortexAnalysisPayload& Payload);

private:
	TSharedRef<SDockTab> SpawnChatTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnQATab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnGenTab(const FSpawnTabArgs& Args);
	void CleanupConversionTab(FName TabId);
	void CleanupAnalysisTab(FName TabId);
	void OnSidebarToggle();
	float GetSidebarSlotValue() const;

	TSharedPtr<FTabManager> TabManager;
	TWeakPtr<FCortexCliSession> SessionWeak;
	TSharedPtr<SCortexSidebar> Sidebar;
	TSharedPtr<SBox> SidebarBox;

	TMap<FName, TSharedPtr<FCortexConversionContext>> ConversionContexts;
	TMap<FName, TSharedPtr<FCortexAnalysisContext>> AnalysisContexts;

	float CachedSidebarCoefficient = 0.20f;
	bool bSidebarCollapsed = false;
};
