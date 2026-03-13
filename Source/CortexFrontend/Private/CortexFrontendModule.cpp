#include "CortexFrontendModule.h"

#include "Framework/Docking/TabManager.h"
#include "IToolMenusModule.h"
#include "Session/CortexCliSession.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/SCortexChatPanel.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY(LogCortexFrontend);

const FName FCortexFrontendModule::CortexChatTabId(TEXT("CortexChat"));

void FCortexFrontendModule::StartupModule()
{
    UE_LOG(LogCortexFrontend, Log, TEXT("CortexFrontend module starting up"));

    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        CortexChatTabId,
        FOnSpawnTab::CreateRaw(this, &FCortexFrontendModule::SpawnChatTab))
        .SetDisplayName(FText::FromString(TEXT("Cortex AI Chat")))
        .SetTooltipText(FText::FromString(TEXT("Open the Cortex AI Chat panel")))
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

    StartupCallbackHandle = UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
        {
            UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
            FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("Cortex"));
            Section.AddEntry(FToolMenuEntry::InitMenuEntry(
                TEXT("CortexChat"),
                FText::FromString(TEXT("AI Chat")),
                FText::FromString(TEXT("Open Cortex AI Chat panel")),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([]()
                {
                    FGlobalTabmanager::Get()->TryInvokeTab(CortexChatTabId);
                }))));
        }));

    UE_LOG(LogCortexFrontend, Log, TEXT("CortexFrontend registered tab and menu"));
}

void FCortexFrontendModule::ShutdownModule()
{
    UE_LOG(LogCortexFrontend, Log, TEXT("CortexFrontend module shutting down"));

    ReleaseSessions();

    if (IToolMenusModule::IsAvailable())
    {
        UToolMenus::UnRegisterStartupCallback(StartupCallbackHandle);
    }

    if (FSlateApplication::IsInitialized())
    {
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CortexChatTabId);
    }
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
    return SNew(SDockTab)
        .TabRole(NomadTab)
        [
            SNew(SCortexChatPanel)
        ];
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

IMPLEMENT_MODULE(FCortexFrontendModule, CortexFrontend)
