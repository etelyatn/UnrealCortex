#include "CortexReplayModule.h"

#include "CortexCoreModule.h"
#include "CortexReplayCommandHandler.h"
#include "CortexReplayInterruptionToast.h"
#include "CortexReplayService.h"
#include "CoreGlobals.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "ICortexCommandRegistry.h"
#include "IToolMenusModule.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SCortexReplayWindow.h"

DEFINE_LOG_CATEGORY(LogCortexReplay);

#define LOCTEXT_NAMESPACE "CortexReplay"

const FName FCortexReplayModule::ReplayTabId(TEXT("CortexReplay"));

namespace
{
const TCHAR* ReplayToolsMenuName = TEXT("LevelEditor.MainMenu.Tools");
const TCHAR* ReplayToolbarMenuName = TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar");
const TCHAR* ReplayMenuSectionName = TEXT("Cortex");
const TCHAR* ReplayEntryName = TEXT("CortexReplay");
const TCHAR* ReplayIconName = TEXT("Icons.Play");
} // namespace

void FCortexReplayModule::StartupModule()
{
	UE_LOG(LogCortexReplay, Log, TEXT("CortexReplay module starting up"));

	ICortexCommandRegistry& Registry =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
		.GetCommandRegistry();

	// The service is the single native owner of replay orchestration; the transport handler only
	// forwards the five AI operations to it, and the human window shares this same owner.
	ReplayService = MakeShared<FCortexReplayService>(FPaths::ProjectDir());
	InterruptionToast = MakeShared<FCortexReplayInterruptionToast>(ReplayService);
	RunTerminalHandle = ReplayService->OnRunTerminal().AddSP(
		InterruptionToast.ToSharedRef(), &FCortexReplayInterruptionToast::Present);

	Registry.RegisterDomain(
		TEXT("replay"),
		TEXT("Cortex Replay"),
		TEXT("1.0.0"),
		MakeShared<FCortexReplayCommandHandler>(ReplayService));

	RegisterTabSpawner();
	RegisterToolsMenuEntry();
	RegisterEditorToolbarEntry();

	// Register for PreExit cleanup so menus/tabs are not touched after engine exit begins.
	FCoreDelegates::OnPreExit.AddRaw(this, &FCortexReplayModule::HandlePreExit);

	UE_LOG(LogCortexReplay, Log, TEXT("CortexReplay registered with CortexCore"));
}

void FCortexReplayModule::ShutdownModule()
{
	UE_LOG(LogCortexReplay, Log, TEXT("CortexReplay module shutting down"));

	FCoreDelegates::OnPreExit.RemoveAll(this);

	UnregisterMenuEntries();
	UnregisterTabSpawner();

	// The toast goes first: its callbacks reach the service, and a service that is shutting down raises
	// no further terminal notifications.
	if (ReplayService.IsValid())
	{
		ReplayService->OnRunTerminal().Remove(RunTerminalHandle);
	}
	RunTerminalHandle.Reset();
	InterruptionToast.Reset();

	// Module-unload barrier: no callback into this module may outlive it (an explicit unload or hot
	// reload frees the code). Owned input/PIE is neutralized and the single terminal result is
	// written now; an owned PIE still ending is kept alive by CortexEditor.
	if (ReplayService.IsValid())
	{
		ReplayService->ShutdownForModuleUnload();
		ReplayService.Reset();
	}
}

void FCortexReplayModule::RegisterTabSpawner()
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		ReplayTabId,
		FOnSpawnTab::CreateRaw(this, &FCortexReplayModule::SpawnReplayTab))
		.SetDisplayName(LOCTEXT("ReplayTabTitle", "Cortex Replay"))
		.SetTooltipText(LOCTEXT("ReplayTabTooltip",
			"Record and replay human gameplay and UI interactions"))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), ReplayIconName))
		// The window is reachable from the Tools menu and the editor toolbar, not the menu list.
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FCortexReplayModule::UnregisterTabSpawner()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ReplayTabId);
	}
}

void FCortexReplayModule::RegisterToolsMenuEntry()
{
	ToolsMenuStartupCallbackHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
		{
			FToolMenuOwnerScoped OwnerScoped(this);
			UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(ReplayToolsMenuName);
			FToolMenuSection& Section = Menu->FindOrAddSection(ReplayMenuSectionName);
			Section.AddEntry(FToolMenuEntry::InitMenuEntry(
				ReplayEntryName,
				LOCTEXT("ReplayMenuLabel", "Cortex Replay"),
				LOCTEXT("ReplayMenuTooltip", "Open the Cortex replay recording library"),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), ReplayIconName),
				FUIAction(FExecuteAction::CreateRaw(this, &FCortexReplayModule::OpenReplayWindow))));
		}));
}

void FCortexReplayModule::RegisterEditorToolbarEntry()
{
	ToolbarStartupCallbackHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
		{
			FToolMenuOwnerScoped OwnerScoped(this);
			UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu(ReplayToolbarMenuName);
			FToolMenuSection& Section = Toolbar->FindOrAddSection(ReplayMenuSectionName);
			Section.AddEntry(FToolMenuEntry::InitToolBarButton(
				ReplayEntryName,
				FExecuteAction::CreateRaw(this, &FCortexReplayModule::OpenReplayWindow),
				LOCTEXT("ReplayToolbarLabel", "Cortex Replay"),
				LOCTEXT("ReplayToolbarTooltip", "Open the Cortex replay recording library"),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), ReplayIconName)));
		}));
}

void FCortexReplayModule::UnregisterMenuEntries()
{
	if (!IToolMenusModule::IsAvailable())
	{
		return;
	}

	UToolMenus::UnRegisterStartupCallback(ToolsMenuStartupCallbackHandle);
	UToolMenus::UnRegisterStartupCallback(ToolbarStartupCallbackHandle);

	// Never traverse destroyed menus once the engine exit has been requested.
	if (!IsEngineExitRequested() && !bHasHandledPreExit)
	{
		UToolMenus::UnregisterOwner(this);
	}
}

void FCortexReplayModule::HandlePreExit()
{
	bHasHandledPreExit = true;
}

void FCortexReplayModule::OpenReplayWindow()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->TryInvokeTab(ReplayTabId);
	}
}

TSharedRef<SDockTab> FCortexReplayModule::SpawnReplayTab(const FSpawnTabArgs& /*Args*/)
{
	// The window observes the shared backend owner; closing the tab never stops a backend run.
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SCortexReplayWindow).Service(ReplayService)
		];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FCortexReplayModule, CortexReplay)
