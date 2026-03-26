#pragma once

#include "CoreMinimal.h"
#include "Analysis/CortexAnalysisContext.h"
#include "Conversion/CortexConversionContext.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

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

	/** Spawn a new chat tab with a fresh session. */
	void SpawnNewChatTab();

private:
	TSharedRef<SDockTab> SpawnChatTab(const FSpawnTabArgs& Args);
	void CleanupConversionTab(FName TabId);
	void CleanupAnalysisTab(FName TabId);
	void CleanupChatTab(FName TabId);
	TSharedRef<SDockTab> BuildChatTab(TSharedPtr<FCortexCliSession> Session, const FString& Label, FName TabId);

	TSharedPtr<FTabManager> TabManager;
	TWeakPtr<FCortexCliSession> SessionWeak;

	TMap<FName, TSharedPtr<FCortexConversionContext>> ConversionContexts;
	TMap<FName, TSharedPtr<FCortexAnalysisContext>> AnalysisContexts;
	TMap<FName, TSharedPtr<FCortexCliSession>> ChatSessions;
	int32 ChatTabCounter = 0;
};
