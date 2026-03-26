#define LOCTEXT_NAMESPACE "CortexFrontend"

#include "CortexFrontendModule.h"

#include "Analysis/CortexAnalysisContext.h"
#include "Conversion/CortexConversionContext.h"
#include "CortexCoreModule.h"
#include "CortexAnalysisTypes.h"
#include "CortexConversionTypes.h"
#include "Framework/Docking/TabManager.h"
#include "IToolMenusModule.h"
#include "Misc/CoreDelegates.h"
#include "Misc/MonitoredProcess.h"
#include "Rendering/CortexRichTextStyle.h"
#include "Session/CortexCliSession.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/SCortexWorkbench.h"
#include "Widgets/SCortexGenPanel.h"
#include "Widgets/SCortexConversionTab.h"
#include "Widgets/SCortexAnalysisTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Text/STextBlock.h"
#include "CortexGenModule.h"
#include "CortexGenSettings.h"

DEFINE_LOG_CATEGORY(LogCortexFrontend);

const FName FCortexFrontendModule::CortexChatTabId(TEXT("CortexFrontend"));
const FName FCortexFrontendModule::GenStudioTabId(TEXT("CortexGenStudio"));

void FCortexFrontendModule::StartupModule()
{
    UE_LOG(LogCortexFrontend, Log, TEXT("CortexFrontend module starting up"));

    FCortexRichTextStyle::Initialize();

    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        CortexChatTabId,
        FOnSpawnTab::CreateRaw(this, &FCortexFrontendModule::SpawnChatTab))
        .SetDisplayName(FText::FromString(TEXT("Cortex Chat")))
        .SetTooltipText(FText::FromString(TEXT("Open the Cortex Chat panel")))
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

    // Register CortexGen Studio Nomad Tab (only if gen module enabled)
    const UCortexGenSettings* GenSettings = UCortexGenSettings::Get();
    if (!GenSettings || GenSettings->bEnabled)
    {
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
            GenStudioTabId,
            FOnSpawnTab::CreateRaw(this, &FCortexFrontendModule::SpawnGenStudioTab))
            .SetDisplayName(LOCTEXT("CortexGenStudio", "CortexGen Studio"))
            .SetMenuType(ETabSpawnerMenuType::Enabled)
            .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());
        bGenStudioTabRegistered = true;
    }

    StartupCallbackHandle = UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
        {
            UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
            FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("Cortex"));
            Section.AddEntry(FToolMenuEntry::InitMenuEntry(
                TEXT("CortexChat"),
                FText::FromString(TEXT("Cortex Chat")),
                FText::FromString(TEXT("Open Cortex Chat panel")),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([]()
                {
                    FGlobalTabmanager::Get()->TryInvokeTab(CortexChatTabId);
                }))));
            // Only show Asset Generation menu if gen Studio tab was registered
            if (bGenStudioTabRegistered)
            {
                Section.AddMenuEntry(
                    TEXT("CortexGenStudio"),
                    LOCTEXT("CortexGenStudioMenuLabel", "Asset Generation"),
                    LOCTEXT("CortexGenStudioMenuTooltip", "Open CortexGen Studio for AI-powered asset generation"),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateLambda([]()
                    {
                        FGlobalTabmanager::Get()->TryInvokeTab(FName("CortexGenStudio"));
                    })));
            }
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
        if (bGenStudioTabRegistered)
        {
            FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GenStudioTabId);
        }
    }

    FCortexRichTextStyle::Shutdown();
}

FCortexSessionConfig FCortexFrontendModule::CreateDefaultSessionConfig()
{
    FCortexSessionConfig Config;
    Config.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    Config.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

    const FString McpPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));
    if (FPaths::FileExists(McpPath))
    {
        Config.McpConfigPath = FPaths::ConvertRelativePathToFull(McpPath);
    }

    return Config;
}

TWeakPtr<FCortexCliSession> FCortexFrontendModule::GetOrCreateSession()
{
    if (Sessions.Num() > 0 && Sessions[0].IsValid())
    {
        return Sessions[0];
    }

    TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(CreateDefaultSessionConfig());
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

    DockTab->SetContent(Workbench);
    return DockTab;
}

TSharedRef<SDockTab> FCortexFrontendModule::SpawnGenStudioTab(const FSpawnTabArgs& Args)
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen")))
    {
        return SNew(SDockTab)
            .TabRole(NomadTab)
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("CortexGen module is not loaded. Check build output for errors.")))
                .ColorAndOpacity(FLinearColor(1.f, 0.3f, 0.3f))
            ];
    }

    if (!FCortexGenModule::IsEnabled())
    {
        return SNew(SDockTab)
            .TabRole(NomadTab)
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("CortexGen is disabled. Enable it in Project Settings → Unreal Cortex → Cortex Gen, then restart the editor.")))
                .ColorAndOpacity(FLinearColor(1.f, 0.7f, 0.2f))
            ];
    }

    TSharedRef<SDockTab> Tab = SNew(SDockTab)
        .TabRole(NomadTab);

    TSharedRef<SCortexGenPanel> Panel = SNew(SCortexGenPanel);

    Tab->SetCanCloseTab(SDockTab::FCanCloseTab::CreateLambda(
        [PanelWeak = TWeakPtr<SCortexGenPanel>(Panel)]()
    {
        TSharedPtr<SCortexGenPanel> PanelPin = PanelWeak.Pin();
        if (PanelPin.IsValid() && PanelPin->HasActiveJobs())
        {
            return true; // Allow close for now
        }
        return true;
    }));

    Tab->SetContent(Panel);
    return Tab;
}

void FCortexFrontendModule::OnConversionRequested(const FCortexConversionPayload& Payload)
{
    TSharedPtr<FCortexConversionContext> Context = MakeShared<FCortexConversionContext>(Payload);

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(FText::FromString(
            FString::Printf(TEXT("%s \u2014 Convert"), *Payload.BlueprintName)))
        .ClientSize(FVector2D(900.0f, 700.0f))
        .SupportsMinimize(true)
        .SupportsMaximize(true)
        [
            SNew(SCortexConversionTab)
            .Context(Context)
        ];

    FConversionWindowEntry Entry;
    Entry.Context = Context;
    Entry.Window = Window;
    ConversionWindows.Add(Entry);

    if (Context->Session.IsValid())
    {
        RegisterSession(Context->Session);
    }

    Window->SetOnWindowClosed(FOnWindowClosed::CreateRaw(
        this, &FCortexFrontendModule::OnConversionWindowClosed, Context));

    FSlateApplication::Get().AddWindow(Window);
}

void FCortexFrontendModule::OnConversionWindowClosed(
    const TSharedRef<SWindow>&,
    TSharedPtr<FCortexConversionContext> Context)
{
    if (Context.IsValid() && Context->Session.IsValid())
    {
        Context->Session->Shutdown();
        UnregisterSession(Context->Session);
    }
    ConversionWindows.RemoveAll([&Context](const FConversionWindowEntry& E)
    {
        return E.Context == Context;
    });
}

void FCortexFrontendModule::OnAnalysisRequested(const FCortexAnalysisPayload& Payload)
{
    TSharedPtr<FCortexAnalysisContext> Context = MakeShared<FCortexAnalysisContext>(Payload);

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(FText::FromString(
            FString::Printf(TEXT("%s \u2014 Analyze"), *Payload.BlueprintName)))
        .ClientSize(FVector2D(900.0f, 700.0f))
        .SupportsMinimize(true)
        .SupportsMaximize(true)
        [
            SNew(SCortexAnalysisTab)
            .Context(Context)
        ];

    FAnalysisWindowEntry Entry;
    Entry.Context = Context;
    Entry.Window = Window;
    AnalysisWindows.Add(Entry);

    if (Context->Session.IsValid())
    {
        RegisterSession(Context->Session);
    }

    Window->SetOnWindowClosed(FOnWindowClosed::CreateRaw(
        this, &FCortexFrontendModule::OnAnalysisWindowClosed, Context));

    FSlateApplication::Get().AddWindow(Window);
}

void FCortexFrontendModule::OnAnalysisWindowClosed(
    const TSharedRef<SWindow>&,
    TSharedPtr<FCortexAnalysisContext> Context)
{
    if (Context.IsValid() && Context->Session.IsValid())
    {
        Context->Session->Shutdown();
        UnregisterSession(Context->Session);
    }
    AnalysisWindows.RemoveAll([&Context](const FAnalysisWindowEntry& E)
    {
        return E.Context == Context;
    });
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

void FCortexFrontendModule::RegisterBuildProcess(TSharedPtr<FMonitoredProcess> Process)
{
    if (Process.IsValid())
    {
        BuildProcesses.AddUnique(Process);
    }
}

void FCortexFrontendModule::UnregisterBuildProcess(TSharedPtr<FMonitoredProcess> Process)
{
    BuildProcesses.Remove(Process);
}

void FCortexFrontendModule::ReleaseSessions()
{
    // Close conversion windows — unbind OnWindowClosed BEFORE RequestDestroyWindow
    // to prevent re-entrancy (RequestDestroyWindow fires the delegate synchronously,
    // which would mutate ConversionWindows while we iterate it)
    for (const FConversionWindowEntry& Entry : ConversionWindows)
    {
        if (TSharedPtr<SWindow> Window = Entry.Window.Pin())
        {
            Window->SetOnWindowClosed(FOnWindowClosed());  // Unbind to prevent re-entrancy
            Window->RequestDestroyWindow();
        }
        // Shut down session directly (since we unbound the close handler)
        if (Entry.Context.IsValid() && Entry.Context->Session.IsValid())
        {
            Entry.Context->Session->Shutdown();
        }
    }
    ConversionWindows.Empty();

    // Close analysis windows — same re-entrancy guard
    for (const FAnalysisWindowEntry& Entry : AnalysisWindows)
    {
        if (TSharedPtr<SWindow> Window = Entry.Window.Pin())
        {
            Window->SetOnWindowClosed(FOnWindowClosed());
            Window->RequestDestroyWindow();
        }
        if (Entry.Context.IsValid() && Entry.Context->Session.IsValid())
        {
            Entry.Context->Session->Shutdown();
        }
    }
    AnalysisWindows.Empty();

    // Cancel all build processes before releasing sessions
    for (const TSharedPtr<FMonitoredProcess>& Process : BuildProcesses)
    {
        if (Process.IsValid())
        {
            Process->Cancel(true);
        }
    }
    BuildProcesses.Reset();

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

#undef LOCTEXT_NAMESPACE
