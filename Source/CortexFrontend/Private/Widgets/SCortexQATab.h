// Source/CortexFrontend/Private/Widgets/SCortexQATab.h
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

struct FCortexSessionStateChange;
struct FCortexSessionConfig;
class FCortexCliSession;
class FCortexQASessionManager;
class SCortexQAToolbar;
class SCortexQASessionList;
class SCortexQADetailPanel;
class SCortexQACommandBar;
struct FCortexTurnResult;

#if WITH_DEV_AUTOMATION_TESTS
struct FCortexQATabSessionCreateResult
{
    TSharedPtr<FCortexCliSession> Session;
    bool bConnected = false;
};

struct FCortexQATabTestHooks
{
    static void SetSessionCreationOverrideForTests(TFunction<FCortexQATabSessionCreateResult(const FCortexSessionConfig&)> InOverride);
    static void ClearSessionCreationOverrideForTests();
};
#endif

class SCortexQATab : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexQATab) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SCortexQATab();

#if WITH_DEV_AUTOMATION_TESTS
    void InvokeGenerateForTests(const FString& Prompt) { OnGenerateClicked(Prompt); }
#endif

private:
    void OnDomainProgress(const FName& DomainName, const TSharedPtr<FJsonObject>& Data);
    void OnSessionSelected(int32 Index);
    void OnRecordConfirmed(const FString& SessionName);
    void OnStopClicked();
    void OnStopAndReplayClicked();
    void OnReplayClicked();
    void OnFastReplayClicked();
    void OnCancelReplayClicked();
    void StartReplay(const FString& ReplayMode);
    void OnDeleteClicked();
    void OnGenerateClicked(const FString& Prompt);
    void OnAIGenerationComplete(const FCortexTurnResult& Result);
    void OnQASessionStateChanged(const FCortexSessionStateChange& StateChange);
    void RefreshSessions();

    TSharedPtr<FCortexQASessionManager> SessionManager;
    TSharedPtr<SCortexQAToolbar> Toolbar;
    TSharedPtr<SCortexQASessionList> SessionList;
    TSharedPtr<SCortexQADetailPanel> DetailPanel;
    TSharedPtr<SCortexQACommandBar> CommandBar;

    TSharedPtr<FCortexCliSession> QACliSession;
    void CancelPendingPIEReplay();

    FDelegateHandle DomainProgressHandle;
    FDelegateHandle PostPIEStartedHandle;
    FTimerHandle PIEStartTimeoutHandle;
    int32 SelectedSessionIndex = INDEX_NONE;
    bool bReplayPendingPIE = false;
    FString PendingReplayMode = TEXT("smooth");
};
