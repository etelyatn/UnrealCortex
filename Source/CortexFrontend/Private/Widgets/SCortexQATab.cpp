// Source/CortexFrontend/Private/Widgets/SCortexQATab.cpp
#include "Widgets/SCortexQATab.h"
#include "Widgets/SCortexQACommandBar.h"
#include "Widgets/SCortexQADetailPanel.h"
#include "Widgets/SCortexQASessionList.h"
#include "Widgets/SCortexQAToolbar.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "CortexCoreDelegates.h"
#include "CortexFrontendModule.h"
#include "Dom/JsonObject.h"
#include "QA/CortexQASessionManager.h"
#include "QA/CortexQATabTypes.h"
#include "Session/CortexCliSession.h"
#include "Session/CortexSessionTypes.h"
#include "Async/Async.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SBox.h"

void SCortexQATab::Construct(const FArguments& InArgs)
{
    // Initialize session manager
    const FString RecordingsDir = FPaths::ProjectSavedDir() / TEXT("CortexQA") / TEXT("Recordings");
    SessionManager = MakeShared<FCortexQASessionManager>(RecordingsDir);
    SessionManager->RefreshSessionList();

    // Subscribe to domain progress
    FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    DomainProgressHandle = Core.OnDomainProgress().AddSP(this, &SCortexQATab::OnDomainProgress);

    // Build layout: Toolbar + Splitter(SessionList | DetailPanel) + CommandBar
    ChildSlot
    [
        SNew(SVerticalBox)

        // Toolbar
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.f)
        [
            SAssignNew(Toolbar, SCortexQAToolbar)
            .OnRecordConfirmed(FOnQARecordConfirmed::CreateSP(this, &SCortexQATab::OnRecordConfirmed))
            .OnStop(FOnQAStopClicked::CreateSP(this, &SCortexQATab::OnStopClicked))
            .OnStopAndReplay(FOnQAStopAndReplayClicked::CreateSP(this, &SCortexQATab::OnStopAndReplayClicked))
        ]

        // Main content: session list | detail panel
        + SVerticalBox::Slot()
        .FillHeight(1.f)
        [
            SNew(SSplitter)
            .Orientation(EOrientation::Orient_Horizontal)

            + SSplitter::Slot()
            .Value(0.35f)
            [
                SAssignNew(SessionList, SCortexQASessionList)
                .OnSessionSelected(FOnQASessionSelected::CreateSP(this, &SCortexQATab::OnSessionSelected))
                .OnSessionDeleted(FOnQASessionDeleted::CreateLambda([this](int32 Index)
                {
                    SessionManager->DeleteSession(Index);
                    RefreshSessions();
                }))
            ]

            + SSplitter::Slot()
            .Value(0.65f)
            [
                SAssignNew(DetailPanel, SCortexQADetailPanel)
                .OnReplay(FOnQAReplayClicked::CreateSP(this, &SCortexQATab::OnReplayClicked))
                .OnDelete(FOnQADeleteClicked::CreateSP(this, &SCortexQATab::OnDeleteClicked))
            ]
        ]

        // Command bar
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.f)
        [
            SAssignNew(CommandBar, SCortexQACommandBar)
            .OnGenerate(FOnQAGenerateClicked::CreateSP(this, &SCortexQATab::OnGenerateClicked))
        ]
    ];

    RefreshSessions();
}

SCortexQATab::~SCortexQATab()
{
    // Unsubscribe from domain progress
    if (DomainProgressHandle.IsValid())
    {
        if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
        {
            FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
            Core.OnDomainProgress().Remove(DomainProgressHandle);
        }
    }

    // Shutdown CLI session if active
    if (QACliSession.IsValid())
    {
        QACliSession->Shutdown();
        if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexFrontend")))
        {
            FCortexFrontendModule& Frontend = FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
            Frontend.UnregisterSession(QACliSession);
        }
    }
}

void SCortexQATab::OnDomainProgress(const FName& DomainName, const TSharedPtr<FJsonObject>& Data)
{
    if (DomainName != FName(TEXT("qa")) || !Data.IsValid())
    {
        return;
    }

    const FString Type = Data->GetStringField(TEXT("type"));

    if (Type == TEXT("session_saved"))
    {
        RefreshSessions();
    }
    else if (Type == TEXT("recording_step") && Toolbar.IsValid())
    {
        const FString StepType = Data->GetStringField(TEXT("step_type"));
        FString Target;
        Data->TryGetStringField(TEXT("target"), Target);
        Toolbar->AddRecordingStep(StepType, Target);
    }
    else if (Type == TEXT("replay_progress") && DetailPanel.IsValid())
    {
        const int32 CurrentStep = static_cast<int32>(Data->GetNumberField(TEXT("current_step")));
        DetailPanel->SetReplayProgress(CurrentStep);
    }
    else if (Type == TEXT("step_completed") && DetailPanel.IsValid())
    {
        // Refresh detail panel to show step results
        DetailPanel->SetReplayProgress(
            static_cast<int32>(Data->GetNumberField(TEXT("step_index"))) + 1);
    }
}

void SCortexQATab::OnSessionSelected(int32 Index)
{
    SelectedSessionIndex = Index;

    if (!DetailPanel.IsValid())
    {
        return;
    }

    if (Index == INDEX_NONE || !SessionManager->GetSessions().IsValidIndex(Index))
    {
        DetailPanel->Clear();
        return;
    }

    const TArray<FCortexQASessionListItem>& Sessions = SessionManager->GetSessions();
    TArray<FCortexQADetailStep> Steps = SessionManager->LoadSteps(Index);
    DetailPanel->SetSession(&Sessions[Index], Steps);
}

void SCortexQATab::OnRecordConfirmed(const FString& SessionName)
{
    FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    FCortexCommandRouter& Router = Core.GetCommandRouter();

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), SessionName);

    FCortexCommandResult Result = Router.Execute(TEXT("qa.start_recording"), Params);

    if (Result.bSuccess)
    {
        if (Toolbar.IsValid())
        {
            Toolbar->SetRecording(true);
        }
        if (SessionList.IsValid())
        {
            SessionList->SetEnabled(false);
        }
    }
    else
    {
        UE_LOG(LogCortexFrontend, Warning, TEXT("Failed to start recording: %s"), *Result.ErrorMessage);
    }
}

void SCortexQATab::OnStopClicked()
{
    FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    FCortexCommandRouter& Router = Core.GetCommandRouter();

    Router.Execute(TEXT("qa.stop_recording"), MakeShared<FJsonObject>());

    if (Toolbar.IsValid())
    {
        Toolbar->SetRecording(false);
    }
    if (SessionList.IsValid())
    {
        SessionList->SetEnabled(true);
    }

    RefreshSessions();
}

void SCortexQATab::OnStopAndReplayClicked()
{
    FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    FCortexCommandRouter& Router = Core.GetCommandRouter();

    FCortexCommandResult StopResult = Router.Execute(TEXT("qa.stop_recording"), MakeShared<FJsonObject>());

    if (Toolbar.IsValid())
    {
        Toolbar->SetRecording(false);
    }
    if (SessionList.IsValid())
    {
        SessionList->SetEnabled(true);
    }

    RefreshSessions();

    if (!StopResult.bSuccess || !StopResult.Data.IsValid())
    {
        return;
    }

    const FString SavedPath = StopResult.Data->GetStringField(TEXT("path"));
    const TArray<FCortexQASessionListItem>& AllSessions = SessionManager->GetSessions();
    for (int32 i = 0; i < AllSessions.Num(); i++)
    {
        if (AllSessions[i].FilePath == SavedPath)
        {
            OnSessionSelected(i);
            OnReplayClicked();
            break;
        }
    }
}

void SCortexQATab::OnReplayClicked()
{
    if (SelectedSessionIndex == INDEX_NONE)
    {
        return;
    }

    const TArray<FCortexQASessionListItem>& Sessions = SessionManager->GetSessions();
    if (!Sessions.IsValidIndex(SelectedSessionIndex))
    {
        return;
    }

    FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    FCortexCommandRouter& Router = Core.GetCommandRouter();

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("path"), Sessions[SelectedSessionIndex].FilePath);
    Params->SetStringField(TEXT("on_failure"), TEXT("continue"));

    TWeakPtr<SCortexQATab> WeakSelf = SharedThis(this);
    Router.Execute(TEXT("qa.replay_session"), Params,
        [WeakSelf](FCortexCommandResult Result)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakSelf, Result]()
            {
                TSharedPtr<SCortexQATab> Self = WeakSelf.Pin();
                if (!Self.IsValid())
                {
                    return;
                }
                if (Self->Toolbar.IsValid())
                {
                    Self->Toolbar->SetPIEStatus(Result.bSuccess ? TEXT("Replay Complete") : TEXT("Replay Failed"));
                }
                Self->RefreshSessions();
            });
        });

    if (Toolbar.IsValid())
    {
        Toolbar->SetPIEStatus(TEXT("Replaying..."));
    }
}

void SCortexQATab::OnDeleteClicked()
{
    if (SelectedSessionIndex != INDEX_NONE)
    {
        SessionManager->DeleteSession(SelectedSessionIndex);
        SelectedSessionIndex = INDEX_NONE;
    }
}

void SCortexQATab::OnGenerateClicked(const FString& Prompt)
{
    // Lazy-create CLI session on first use
    if (!QACliSession.IsValid())
    {
        FCortexSessionConfig Config;
        Config.SessionId = TEXT("cortex-qa-session");
        Config.SystemPrompt = TEXT(
            "You are a QA test engineer agent. Generate test scenarios as JSON step arrays.\n"
            "Available step types: position_snapshot, move_to, interact, look_at, wait, assert, key_press.\n"
            "Use MCP tools to inspect the level and determine actor paths.\n"
            "Save the generated scenario to Saved/CortexQA/Recordings/ using the session JSON format."
        );
        Config.bSkipPermissions = true;
        Config.bConversionMode = false;

        QACliSession = MakeShared<FCortexCliSession>(Config);
        QACliSession->Connect();

        FCortexFrontendModule& Frontend = FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
        Frontend.RegisterSession(QACliSession);

        QACliSession->OnTurnComplete.AddSP(this, &SCortexQATab::OnAIGenerationComplete);
    }

    if (CommandBar.IsValid())
    {
        CommandBar->SetGenerating(true);
    }

    FCortexPromptRequest Request;
    Request.Prompt = Prompt;
    QACliSession->SendPrompt(Request);
}

void SCortexQATab::OnAIGenerationComplete(const FCortexTurnResult& /*Result*/)
{
    RefreshSessions();

    // Auto-select the newest session (index 0 after refresh, sorted by date desc)
    if (SessionManager->GetSessions().Num() > 0)
    {
        OnSessionSelected(0);
    }

    if (CommandBar.IsValid())
    {
        CommandBar->SetGenerating(false);
    }
}

void SCortexQATab::RefreshSessions()
{
    SessionManager->RefreshSessionList();
    if (SessionList.IsValid())
    {
        SessionList->SetSessions(SessionManager->GetSessions());
    }
}
