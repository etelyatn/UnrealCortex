#include "Misc/AutomationTest.h"
#include "CortexFrontendProviderSettings.h"
#include "CortexFrontendSettings.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Session/CortexCliSession.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReconnectRejectsNonIdleTest,
    "Cortex.Frontend.ContextControls.Reconnect.RejectsNonIdle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReconnectRejectsNonIdleTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-reconnect-reject");
    TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(Config);

    TestFalse(TEXT("Rejects from Inactive"), Session->Reconnect());

    Session->SetStateForTest(ECortexSessionState::Processing);
    TestFalse(TEXT("Rejects from Processing"), Session->Reconnect());

    Session->SetStateForTest(ECortexSessionState::Spawning);
    TestFalse(TEXT("Rejects from Spawning"), Session->Reconnect());

    Session->SetStateForTest(ECortexSessionState::Cancelling);
    TestFalse(TEXT("Rejects from Cancelling"), Session->Reconnect());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReconnectFromIdleTransitionsTest,
    "Cortex.Frontend.ContextControls.Reconnect.FromIdleTransitions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReconnectFromIdleTransitionsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-reconnect-idle");
    TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(Config);

    Session->SetStateForTest(ECortexSessionState::Idle);

    const bool bResult = Session->Reconnect();

    const ECortexSessionState FinalState = Session->GetStateForTest();

    // State must leave Idle — either succeeds (Idle, CLI found) or fails (Inactive, no CLI).
    // Either way, the session must not remain stuck in Idle and result must match final state.
    const bool bEndedIdle = FinalState == ECortexSessionState::Idle;
    const bool bEndedInactive = FinalState == ECortexSessionState::Inactive;
    TestTrue(TEXT("State transitions away from or returns to a valid terminal state"),
        bEndedIdle || bEndedInactive);
    if (bEndedIdle)
    {
        TestTrue(TEXT("Reconnect returns true when CLI found"), bResult);
    }
    else
    {
        TestFalse(TEXT("Reconnect returns false when CLI not found"), bResult);
        TestEqual(TEXT("State is Inactive after failed reconnect"),
            static_cast<uint8>(FinalState),
            static_cast<uint8>(ECortexSessionState::Inactive));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReconnectDirtyStatePreservedOnFailureTest,
    "Cortex.Frontend.ContextControls.Reconnect.DirtyPreservedOnFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReconnectDirtyStatePreservedOnFailureTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();

    const ECortexEffortLevel Orig = Settings.GetEffortLevel();
    Settings.SetEffortLevel(ECortexEffortLevel::High);
    TestTrue(TEXT("Dirty before reconnect"), Settings.HasPendingChanges());

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-reconnect-dirty");
    TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(Config);
    Session->SetStateForTest(ECortexSessionState::Idle);
    const bool bResult = Session->Reconnect();

    if (!bResult)
    {
        // Reconnect failed (no CLI available): dirty state must be preserved so
        // the banner remains visible and the user can retry.
        TestTrue(TEXT("Dirty state preserved on failed reconnect"), Settings.HasPendingChanges());
    }
    else
    {
        // Reconnect succeeded: dirty state is cleared as settings were applied.
        TestFalse(TEXT("Dirty state cleared on successful reconnect"), Settings.HasPendingChanges());
    }

    Settings.SetEffortLevel(Orig);
    Settings.ClearPendingChanges();
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReconnectPinsLaunchMetadataAcrossSettingsChangeTest,
    "Cortex.Frontend.ContextControls.Reconnect.PinsLaunchMetadataAcrossSettingsChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReconnectPinsLaunchMetadataAcrossSettingsChangeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString TempSettingsPath = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("CortexFrontend"),
        FString::Printf(TEXT("Task4ReconnectTest_%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(TempSettingsPath), true);
    FCortexFrontendSettings::SetSettingsFilePathOverrideForTests(TempSettingsPath);
    ON_SCOPE_EXIT
    {
        FCortexFrontendSettings::ClearSettingsFilePathOverrideForTests();
        IFileManager::Get().Delete(*TempSettingsPath);
    };

    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    UCortexFrontendProviderSettings* ProviderSettings = GetMutableDefault<UCortexFrontendProviderSettings>();
    TestNotNull(TEXT("Provider settings should exist"), ProviderSettings);
    if (!ProviderSettings)
    {
        return false;
    }

    const FString OriginalProviderId = ProviderSettings->ActiveProviderId;
    const ECortexAccessMode OriginalAccessMode = Settings.GetAccessMode();
    const bool OriginalSkipPermissions = Settings.GetSkipPermissions();
    const ECortexWorkflowMode OriginalWorkflow = Settings.GetWorkflowMode();
    const bool OriginalProjectContext = Settings.GetProjectContext();
    const bool OriginalAutoContext = Settings.GetAutoContext();
    const FString OriginalDirective = Settings.GetCustomDirective();
    const ECortexEffortLevel OriginalEffort = Settings.GetEffortLevel();
    const FString OriginalModel = Settings.GetSelectedModel();
    ON_SCOPE_EXIT
    {
        ProviderSettings->ActiveProviderId = OriginalProviderId;
        Settings.SetAccessMode(OriginalAccessMode);
        Settings.SetSkipPermissions(OriginalSkipPermissions);
        Settings.SetWorkflowMode(OriginalWorkflow);
        Settings.SetProjectContext(OriginalProjectContext);
        Settings.SetAutoContext(OriginalAutoContext);
        Settings.SetCustomDirective(OriginalDirective);
        Settings.SetEffortLevel(OriginalEffort);
        Settings.SetSelectedModel(OriginalModel);
        Settings.ClearPendingChanges();
    };

    ProviderSettings->ActiveProviderId = TEXT("codex");
    Settings.SetAccessMode(ECortexAccessMode::Guided);
    Settings.SetSkipPermissions(false);
    Settings.SetWorkflowMode(ECortexWorkflowMode::Thorough);
    Settings.SetProjectContext(true);
    Settings.SetAutoContext(true);
    Settings.SetCustomDirective(TEXT("Reconnect snapshot"));
    Settings.SetEffortLevel(ECortexEffortLevel::Medium);
    Settings.SetSelectedModel(TEXT("gpt-5.4"));

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-reconnect-pinned");
    Config.McpConfigPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));

    TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(Config);
    FCortexStreamEvent InitEvent;
    InitEvent.Type = ECortexStreamEventType::SessionInit;
    InitEvent.SessionId = TEXT("thread-reconnect-123");
    InitEvent.Model = TEXT("gpt-5.4");
    Session->HandleWorkerEvent(InitEvent);
    TestEqual(TEXT("Reconnect session should persist the real thread id"), Session->GetSessionId(), FString(TEXT("thread-reconnect-123")));

    TestTrue(TEXT("Session should pin codex provider"), Session->GetProviderId() == FName(TEXT("codex")));
    TestTrue(TEXT("Session should pin codex model"), Session->GetResolvedOptions().ModelId == TEXT("gpt-5.4"));
    TestTrue(TEXT("Session should pin codex effort"), Session->GetResolvedOptions().EffortLevel == ECortexEffortLevel::Medium);
    TestTrue(TEXT("Session should pin context limit"), Session->GetContextLimitTokens() == static_cast<int64>(272000));

    const FString LaunchBeforeSettingsChange = Session->BuildLaunchCommandLine(true, ECortexAccessMode::Guided);
    TestFalse(TEXT("Pinned reconnect launch should snapshot live skip permissions"), LaunchBeforeSettingsChange.Contains(TEXT("--dangerously-bypass-approvals-and-sandbox")));
    TestTrue(TEXT("Pinned reconnect launch should keep codex model"), LaunchBeforeSettingsChange.Contains(TEXT("-m \"gpt-5.4\"")));

    ProviderSettings->ActiveProviderId = TEXT("claude_code");
    Settings.SetSkipPermissions(true);
    Settings.SetEffortLevel(ECortexEffortLevel::High);
    Settings.SetSelectedModel(TEXT("claude-opus-4-6"));

    Session->SetStateForTest(ECortexSessionState::Idle);
    (void)Session->Reconnect();

    const FString LaunchAfterSettingsChange = Session->BuildLaunchCommandLine(true, ECortexAccessMode::Guided);
    TestEqual(TEXT("Reconnect launch should remain pinned across settings changes"), LaunchAfterSettingsChange, LaunchBeforeSettingsChange);
    TestTrue(TEXT("Reconnect launch should use the real thread id"), LaunchAfterSettingsChange.Contains(TEXT("thread-reconnect-123")));
    TestTrue(TEXT("Pinned provider id should remain codex"), Session->GetProviderId() == FName(TEXT("codex")));
    TestTrue(TEXT("Pinned resolved model should remain gpt-5.4"), Session->GetResolvedOptions().ModelId == TEXT("gpt-5.4"));
    TestTrue(TEXT("Pinned resolved effort should remain medium"), Session->GetResolvedOptions().EffortLevel == ECortexEffortLevel::Medium);
    return true;
}
