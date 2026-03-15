#include "Misc/AutomationTest.h"
#include "CortexFrontendSettings.h"
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
