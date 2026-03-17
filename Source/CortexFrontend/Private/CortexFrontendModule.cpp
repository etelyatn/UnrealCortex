#include "CortexFrontendModule.h"

#include "CortexCoreModule.h"
#include "CortexAnalysisTypes.h"
#include "CortexConversionTypes.h"
#include "Framework/Docking/TabManager.h"
#include "IToolMenusModule.h"
#include "Misc/CoreDelegates.h"
#include "Rendering/CortexRichTextStyle.h"
#include "Session/CortexCliSession.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/SCortexWorkbench.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY(LogCortexFrontend);

const FName FCortexFrontendModule::CortexChatTabId(TEXT("CortexFrontend"));

void FCortexFrontendModule::StartupModule()
{
    UE_LOG(LogCortexFrontend, Log, TEXT("CortexFrontend module starting up"));

    FCortexRichTextStyle::Initialize();

    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        CortexChatTabId,
        FOnSpawnTab::CreateRaw(this, &FCortexFrontendModule::SpawnChatTab))
        .SetDisplayName(FText::FromString(TEXT("Cortex Frontend")))
        .SetTooltipText(FText::FromString(TEXT("Open the Cortex Frontend panel")))
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

    StartupCallbackHandle = UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
        {
            UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
            FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("Cortex"));
            Section.AddEntry(FToolMenuEntry::InitMenuEntry(
                TEXT("CortexChat"),
                FText::FromString(TEXT("Cortex Frontend")),
                FText::FromString(TEXT("Open Cortex Frontend panel")),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([]()
                {
                    FGlobalTabmanager::Get()->TryInvokeTab(CortexChatTabId);
                }))));
        }));

    FCoreDelegates::OnPreExit.AddRaw(this, &FCortexFrontendModule::HandlePreExit);

    // Subscribe to conversion events from CortexBlueprint (via CortexCore)
    if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
    {
        FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
        ConversionDelegateHandle = Core.OnConversionRequested().AddRaw(
            this, &FCortexFrontendModule::OnConversionRequested);
        AnalysisDelegateHandle = Core.OnAnalysisRequested().AddRaw(
            this, &FCortexFrontendModule::OnAnalysisRequested);
    }

    UE_LOG(LogCortexFrontend, Log, TEXT("CortexFrontend registered tab and menu"));
}

void FCortexFrontendModule::ShutdownModule()
{
    UE_LOG(LogCortexFrontend, Log, TEXT("CortexFrontend module shutting down"));

    FCoreDelegates::OnPreExit.RemoveAll(this);

    if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
    {
        FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
        Core.OnConversionRequested().Remove(ConversionDelegateHandle);
    }

    if (AnalysisDelegateHandle.IsValid())
    {
        if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
        {
            FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
            Core.OnAnalysisRequested().Remove(AnalysisDelegateHandle);
        }
    }

    ReleaseSessions();

    if (IToolMenusModule::IsAvailable())
    {
        UToolMenus::UnRegisterStartupCallback(StartupCallbackHandle);
    }

    if (FSlateApplication::IsInitialized())
    {
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CortexChatTabId);
    }

    FCortexRichTextStyle::Shutdown();
}

TWeakPtr<FCortexCliSession> FCortexFrontendModule::GetOrCreateSession()
{
    if (Sessions.Num() > 0 && Sessions[0].IsValid())
    {
        return Sessions[0];
    }

    FCortexSessionConfig Config;
    Config.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    Config.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

    const FString McpPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));
    if (FPaths::FileExists(McpPath))
    {
        Config.McpConfigPath = FPaths::ConvertRelativePathToFull(McpPath);
    }

    TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(Config);
    Sessions.Reset();
    Sessions.Add(Session);
    return Session;
}

TSharedRef<SDockTab> FCortexFrontendModule::SpawnChatTab(const FSpawnTabArgs& /*Args*/)
{
    TSharedRef<SDockTab> DockTab = SNew(SDockTab).TabRole(NomadTab);

    TSharedRef<SCortexWorkbench> Workbench = SNew(SCortexWorkbench)
        .OwnerTab(DockTab)
        .Session(GetOrCreateSession());

    WorkbenchWeak = Workbench;  // Store weak ref for conversion routing
    DockTab->SetContent(Workbench);
    return DockTab;
}

void FCortexFrontendModule::OnConversionRequested(const FCortexConversionPayload& Payload)
{
    // Auto-open the CortexFrontend panel
    FGlobalTabmanager::Get()->TryInvokeTab(CortexChatTabId);

    // Route to workbench via stored weak reference (safe — no static_cast)
    TSharedPtr<SCortexWorkbench> Workbench = WorkbenchWeak.Pin();
    if (Workbench.IsValid())
    {
        Workbench->SpawnConversionTab(Payload);
    }
}

void FCortexFrontendModule::OnAnalysisRequested(const FCortexAnalysisPayload& Payload)
{
    FGlobalTabmanager::Get()->TryInvokeTab(CortexChatTabId);

    TSharedPtr<SCortexWorkbench> Workbench = WorkbenchWeak.Pin();
    if (Workbench.IsValid())
    {
        Workbench->SpawnAnalysisTab(Payload);
    }
}

void FCortexFrontendModule::RegisterSession(TSharedPtr<FCortexCliSession> Session)
{
    if (Session.IsValid())
    {
        Sessions.AddUnique(Session);
    }
}

void FCortexFrontendModule::UnregisterSession(TSharedPtr<FCortexCliSession> Session)
{
    Sessions.Remove(Session);
}

void FCortexFrontendModule::ReleaseSessions()
{
    for (const TSharedPtr<FCortexCliSession>& Session : Sessions)
    {
        if (Session.IsValid())
        {
            Session->Shutdown();
        }
    }

    Sessions.Reset();
}

void FCortexFrontendModule::HandlePreExit()
{
    UE_LOG(LogCortexFrontend, Log, TEXT("PreExit: releasing sessions"));
    ReleaseSessions();
}

IMPLEMENT_MODULE(FCortexFrontendModule, CortexFrontend)
