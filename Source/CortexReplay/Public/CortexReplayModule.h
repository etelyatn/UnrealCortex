#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogCortexReplay, Log, All);

class FCortexReplayService;
class FSpawnTabArgs;
class SDockTab;

/**
 * Editor-only module entry for CortexReplay.
 *
 * Besides registering the replay domain with CortexCore, it owns the independent human
 * recording-library window: a Tools-menu entry, an editor-toolbar entry and one nomad tab
 * spawner. Menu/tab teardown follows the existing safe convention (register through
 * UToolMenus startup callbacks, unsubscribe on FCoreDelegates::OnPreExit and never traverse
 * destroyed menus once the engine exit has been requested).
 */
class FCortexReplayModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Tab id of the independent human recording-library window. */
	static const FName ReplayTabId;

	/** The backend owner shared with the human window; never owns window/socket lifetime. */
	TSharedPtr<FCortexReplayService> GetReplayService() const;

private:
	TSharedRef<SDockTab> SpawnReplayTab(const FSpawnTabArgs& Args);
	void OpenReplayWindow();

	void RegisterToolsMenuEntry();
	void RegisterEditorToolbarEntry();
	void RegisterTabSpawner();

	void UnregisterTabSpawner();
	void UnregisterMenuEntries();

	/** Called before engine exit; drops menu/tab registrations while they are still valid. */
	void HandlePreExit();

	/** True only when module unload may safely call UToolMenus::UnregisterOwner. */
	static bool ShouldTraverseToolMenusDuringShutdown(
		bool bIsEngineExitRequested, bool bIsToolMenusAvailable);

	/** Backend owner for the domain handler and the human window. */
	TSharedPtr<FCortexReplayService> ReplayService;

	FDelegateHandle ToolsMenuStartupCallbackHandle;
	FDelegateHandle ToolbarStartupCallbackHandle;

	/** Set by HandlePreExit so ShutdownModule does not touch destroyed menus late. */
	bool bHasHandledPreExit = false;
};
