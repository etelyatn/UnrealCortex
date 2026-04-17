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
#include "Editor.h"
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
            .OnCancelReplay(FOnQACancelReplayClicked::CreateSP(this, &SCortexQATab::OnCancelReplayClicked))
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
                .OnFastReplay(FOnQAFastReplayClicked::CreateSP(this, &SCortexQATab::OnFastReplayClicked))
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

    // Clean up PIE auto-start state
    CancelPendingPIEReplay();

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
    StartReplay(TEXT("smooth"));
}

void SCortexQATab::OnFastReplayClicked()
{
    StartReplay(TEXT("teleport"));
}

void SCortexQATab::StartReplay(const FString& ReplayMode)
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

    // Check if PIE is active; if not, start it and defer replay
    if (GEditor != nullptr && GEditor->PlayWorld == nullptr)
    {
        bReplayPendingPIE = true;
        PendingReplayMode = ReplayMode;
        TWeakPtr<SCortexQATab> WeakSelf = SharedThis(this);

        // Subscribe to PostPIEStarted to trigger replay once PIE is ready
        if (!PostPIEStartedHandle.IsValid())
        {
            PostPIEStartedHandle = FEditorDelegates::PostPIEStarted.AddLambda(
                [WeakSelf](bool bIsSimulating)
                {
                    AsyncTask(ENamedThreads::GameThread, [WeakSelf]()
                    {
                        TSharedPtr<SCortexQATab> Self = WeakSelf.Pin();
                        if (!Self.IsValid() || !Self->bReplayPendingPIE)
                        {
                            return;
                        }
                        Self->bReplayPendingPIE = false;

                        // Cancel the timeout timer
                        if (GEditor != nullptr)
                        {
                            GEditor->GetTimerManager()->ClearTimer(Self->PIEStartTimeoutHandle);
                        }

                        // Unbind the delegate
                        FEditorDelegates::PostPIEStarted.Remove(Self->PostPIEStartedHandle);
                        Self->PostPIEStartedHandle.Reset();

                        // Delay replay slightly to let PIE fully initialize (player pawn spawn)
                        if (GEditor != nullptr)
                        {
                            TWeakPtr<SCortexQATab> DelayedSelf = Self;
                            const FString Mode = Self->PendingReplayMode;
                            GEditor->GetTimerManager()->SetTimer(
                                Self->PIEStartTimeoutHandle,
                                FTimerDelegate::CreateLambda([DelayedSelf, Mode]()
                                {
                                    if (TSharedPtr<SCortexQATab> S = DelayedSelf.Pin())
                                    {
                                        S->StartReplay(Mode);
                                    }
                                }),
                                1.0f,
                                false);
                        }
                    });
                });
        }

        // Request PIE start
        FRequestPlaySessionParams PlayParams;
        GEditor->RequestPlaySession(PlayParams);

        // Set a timeout in case PIE fails to start (e.g., compile errors)
        GEditor->GetTimerManager()->SetTimer(
            PIEStartTimeoutHandle,
            FTimerDelegate::CreateLambda([WeakSelf]()
            {
                if (TSharedPtr<SCortexQATab> Self = WeakSelf.Pin())
                {
                    if (Self->bReplayPendingPIE)
                    {
                        Self->CancelPendingPIEReplay();
                        if (Self->Toolbar.IsValid())
                        {
                            Self->Toolbar->SetPIEStatus(TEXT("PIE start failed"));
                        }
                    }
                }
            }),
            15.0f,
            false);

        if (Toolbar.IsValid())
        {
            Toolbar->SetPIEStatus(TEXT("Starting PIE..."));
        }
        return;
    }

    const FString& SessionPath = Sessions[SelectedSessionIndex].FilePath;
    UE_LOG(LogCortexFrontend, Log, TEXT("QA Replay: starting %s replay for session '%s'"),
        *ReplayMode, *SessionPath);

    FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    FCortexCommandRouter& Router = Core.GetCommandRouter();

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("path"), SessionPath);
    Params->SetStringField(TEXT("on_failure"), TEXT("continue"));
    Params->SetStringField(TEXT("replay_mode"), ReplayMode);

    TWeakPtr<SCortexQATab> WeakSelf2 = SharedThis(this);
    FCortexCommandResult Result = Router.Execute(TEXT("qa.replay_session"), Params,
        [WeakSelf2](FCortexCommandResult DeferredResult)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakSelf2, DeferredResult]()
            {
                TSharedPtr<SCortexQATab> Self = WeakSelf2.Pin();
                if (!Self.IsValid())
                {
                    return;
                }
                UE_LOG(LogCortexFrontend, Log, TEXT("QA Replay: completed — %s"),
                    DeferredResult.bSuccess ? TEXT("SUCCESS") : *DeferredResult.ErrorMessage);
                if (Self->Toolbar.IsValid())
                {
                    Self->Toolbar->SetReplaying(false);
                    Self->Toolbar->SetPIEStatus(DeferredResult.bSuccess ? TEXT("Replay Complete") : TEXT("Replay Failed"));
                }
                Self->RefreshSessions();
            });
        });

    // Check for synchronous errors (PIE_NOT_ACTIVE, MAP_MISMATCH, etc.)
    if (!Result.bIsDeferred)
    {
        UE_LOG(LogCortexFrontend, Warning, TEXT("QA Replay: failed to start — %s"), *Result.ErrorMessage);
        if (Toolbar.IsValid())
        {
            Toolbar->SetPIEStatus(FString::Printf(TEXT("Replay Error: %s"), *Result.ErrorMessage));
        }
        return;
    }

    if (Toolbar.IsValid())
    {
        Toolbar->SetReplaying(true);
    }
}

void SCortexQATab::OnCancelReplayClicked()
{
    FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    FCortexCommandRouter& Router = Core.GetCommandRouter();

    Router.Execute(TEXT("qa.cancel_replay"), MakeShared<FJsonObject>());

    // The replay completion callback will reset the toolbar state
}

void SCortexQATab::OnDeleteClicked()
{
    if (SelectedSessionIndex != INDEX_NONE)
    {
        SessionManager->DeleteSession(SelectedSessionIndex);
        SelectedSessionIndex = INDEX_NONE;

        if (DetailPanel.IsValid())
        {
            DetailPanel->Clear();
        }
        RefreshSessions();
    }
}

void SCortexQATab::OnGenerateClicked(const FString& Prompt)
{
    // Lazy-create CLI session on first use
    if (!QACliSession.IsValid())
    {
        FCortexSessionConfig Config = FCortexFrontendModule::CreateDefaultSessionConfig();
        Config.SessionId = TEXT("cortex-qa-session");
        Config.SystemPrompt = TEXT(
            "You are a QA test engineer agent. Generate test scenarios as JSON step arrays.\n"
            "Available step types: position_snapshot, move_to, interact, look_at, wait, assert, key_press.\n"
            "Use MCP tools to inspect the level and determine actor paths.\n"
            "Save the generated scenario to Saved/CortexQA/Recordings/ using the session JSON format."
        );
        Config.bConversionMode = false;

        QACliSession = MakeShared<FCortexCliSession>(Config);

        if (!QACliSession->Connect())
        {
            UE_LOG(LogCortexFrontend, Warning, TEXT("QA: Failed to connect CLI session for AI generation"));
            if (CommandBar.IsValid())
            {
                CommandBar->SetStatus(TEXT("Failed to start Claude CLI"));
            }
            QACliSession.Reset();
            return;
        }

        FCortexFrontendModule& Frontend = FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
        Frontend.RegisterSession(QACliSession);

        QACliSession->OnTurnComplete.AddSP(this, &SCortexQATab::OnAIGenerationComplete);
        QACliSession->OnStateChanged.AddSP(this, &SCortexQATab::OnQASessionStateChanged);
    }

    if (CommandBar.IsValid())
    {
        CommandBar->SetGenerating(true);
        CommandBar->SetStatus(TEXT("Generating..."));
    }

    FCortexPromptRequest Request;
    Request.Prompt = Prompt;
    Request.AccessMode = ECortexAccessMode::FullAccess;
    if (!QACliSession->SendPrompt(Request))
    {
        UE_LOG(LogCortexFrontend, Warning, TEXT("QA: SendPrompt failed (session state: %d)"),
            static_cast<int32>(QACliSession->GetState()));

        if (CommandBar.IsValid())
        {
            CommandBar->SetGenerating(false);
            CommandBar->SetStatus(TEXT("Failed to send prompt"));
        }
    }
}

void SCortexQATab::OnAIGenerationComplete(const FCortexTurnResult& Result)
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
        CommandBar->SetStatus(Result.bIsError ? TEXT("Generation failed") : TEXT("Done"));
    }
}

void SCortexQATab::OnQASessionStateChanged(const FCortexSessionStateChange& StateChange)
{
    // If the CLI session dies or goes inactive while generating, re-enable the button
    if (StateChange.NewState == ECortexSessionState::Inactive ||
        StateChange.NewState == ECortexSessionState::Terminated)
    {
        if (CommandBar.IsValid())
        {
            CommandBar->SetGenerating(false);
            if (!StateChange.Reason.IsEmpty())
            {
                CommandBar->SetStatus(StateChange.Reason);
            }
        }
    }
}

void SCortexQATab::CancelPendingPIEReplay()
{
    bReplayPendingPIE = false;

    if (PostPIEStartedHandle.IsValid())
    {
        FEditorDelegates::PostPIEStarted.Remove(PostPIEStartedHandle);
        PostPIEStartedHandle.Reset();
    }

    if (GEditor != nullptr)
    {
        GEditor->GetTimerManager()->ClearTimer(PIEStartTimeoutHandle);
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
