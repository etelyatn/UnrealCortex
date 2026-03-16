// Source/CortexFrontend/Private/Widgets/SCortexQATab.cpp
#include "Widgets/SCortexQATab.h"
#include "Widgets/SCortexQADetailPanel.h"
#include "Widgets/SCortexQASessionList.h"
#include "Widgets/SCortexQAToolbar.h"
#include "CortexCoreModule.h"
#include "CortexCoreDelegates.h"
#include "CortexFrontendModule.h"
#include "Dom/JsonObject.h"
#include "QA/CortexQASessionManager.h"
#include "Session/CortexCliSession.h"
#include "Session/CortexSessionTypes.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

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

        // Command bar placeholder
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.f)
        [
            SNew(STextBlock)
            .Text(FText::FromString(TEXT("[Command Bar Placeholder]")))
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
}

void SCortexQATab::OnSessionSelected(int32 Index)
{
    SelectedSessionIndex = Index;
}

void SCortexQATab::OnRecordConfirmed(const FString& SessionName)
{
    // Stub — full implementation in Task 16
}

void SCortexQATab::OnStopClicked()
{
    // Stub — full implementation in Task 16
}

void SCortexQATab::OnStopAndReplayClicked()
{
    // Stub — full implementation in Task 16
}

void SCortexQATab::OnReplayClicked()
{
    // Stub — full implementation in Task 16
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
}

void SCortexQATab::RefreshSessions()
{
    SessionManager->RefreshSessionList();
    if (SessionList.IsValid())
    {
        SessionList->SetSessions(SessionManager->GetSessions());
    }
}
