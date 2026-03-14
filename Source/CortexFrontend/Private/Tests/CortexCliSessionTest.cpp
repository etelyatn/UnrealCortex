#include "Misc/AutomationTest.h"
#include "CortexFrontendModule.h"
#include "Modules/ModuleManager.h"
#include "Session/CortexCliSession.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionBuildInitialLaunchArgsTest, "Cortex.Frontend.CliSession.BuildInitialLaunchArgs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionBuildResumeLaunchArgsTest, "Cortex.Frontend.CliSession.BuildResumeLaunchArgs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionBuildPromptEnvelopeTest, "Cortex.Frontend.CliSession.BuildPromptEnvelope", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionQueuePromptWhileSpawningTest, "Cortex.Frontend.CliSession.QueuePromptWhileSpawning", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionTurnCompleteReturnsIdleTest, "Cortex.Frontend.CliSession.TurnCompleteReturnsIdle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionCancelTransitionsTest, "Cortex.Frontend.CliSession.CancelTransitions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionNewChatGeneratesFreshSessionIdTest, "Cortex.Frontend.CliSession.NewChatGeneratesFreshSessionId", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFrontendModuleGetOrCreateSessionTest, "Cortex.Frontend.Module.GetOrCreateSession", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliSessionBuildInitialLaunchArgsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("session-123");
    Config.McpConfigPath = TEXT("D:/UnrealProjects/CortexSandbox/.mcp.json");
    Config.bSkipPermissions = true;

    FCortexCliSession Session(Config);
    const FString CommandLine = Session.BuildLaunchCommandLine(false, ECortexAccessMode::Guided);

    TestTrue(TEXT("Input format should be stream-json"), CommandLine.Contains(TEXT("--input-format stream-json")));
    TestTrue(TEXT("Output format should be stream-json"), CommandLine.Contains(TEXT("--output-format stream-json")));
    TestTrue(TEXT("Initial launch should include session id"), CommandLine.Contains(TEXT("--session-id \"session-123\"")));
    TestFalse(TEXT("Initial launch should not include resume"), CommandLine.Contains(TEXT("--resume")));
    TestTrue(TEXT("Initial launch should include MCP config"), CommandLine.Contains(TEXT("--mcp-config \"D:/UnrealProjects/CortexSandbox/.mcp.json\"")));
    TestTrue(TEXT("Initial launch should include allowed tools"), CommandLine.Contains(TEXT("--allowedTools")));
    return true;
}

bool FCortexCliSessionBuildResumeLaunchArgsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("session-456");
    Config.bSkipPermissions = true;

    FCortexCliSession Session(Config);
    const FString CommandLine = Session.BuildLaunchCommandLine(true, ECortexAccessMode::ReadOnly);

    TestTrue(TEXT("Resume launch should include resume"), CommandLine.Contains(TEXT("--resume \"session-456\"")));
    TestFalse(TEXT("Resume launch should not include session id"), CommandLine.Contains(TEXT("--session-id")));
    TestTrue(TEXT("Resume launch should include read-only tool patterns"), CommandLine.Contains(TEXT("mcp__cortex_mcp__get_*")));
    return true;
}

bool FCortexCliSessionBuildPromptEnvelopeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("session-789");

    FCortexCliSession Session(Config);
    const FString Envelope = Session.BuildPromptEnvelope(TEXT("Inspect the selected actor"));

    TestTrue(TEXT("Envelope should encode user message type"), Envelope.Contains(TEXT("\"type\":\"user\"")));
    TestTrue(TEXT("Envelope should encode user role"), Envelope.Contains(TEXT("\"role\":\"user\"")));
    TestTrue(TEXT("Envelope should encode prompt content"), Envelope.Contains(TEXT("\"content\":\"Inspect the selected actor\"")));
    TestTrue(TEXT("Envelope should terminate with newline"), Envelope.EndsWith(TEXT("\n")));
    return true;
}

bool FCortexCliSessionQueuePromptWhileSpawningTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("session-queue");

    FCortexCliSession Session(Config);
    Session.SetStateForTest(ECortexSessionState::Spawning);

    FCortexPromptRequest PromptRequest;
    PromptRequest.Prompt = TEXT("queued prompt");
    PromptRequest.AccessMode = ECortexAccessMode::Guided;

    TestTrue(TEXT("Prompt should be accepted while spawning"), Session.SendPrompt(PromptRequest));
    TestEqual(TEXT("Prompt should remain queued until process ready"), Session.GetPendingPromptForTest(), FString(TEXT("queued prompt")));
    TestEqual(TEXT("Session should remain spawning while prompt is queued"), Session.GetStateForTest(), ECortexSessionState::Spawning);
    return true;
}

bool FCortexCliSessionTurnCompleteReturnsIdleTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("session-turn");

    FCortexCliSession Session(Config);
    Session.SetStateForTest(ECortexSessionState::Processing);

    FCortexTurnResult CapturedResult;
    bool bDelegateCalled = false;
    Session.OnTurnComplete.AddLambda([&CapturedResult, &bDelegateCalled](const FCortexTurnResult& Result)
    {
        CapturedResult = Result;
        bDelegateCalled = true;
    });

    FCortexStreamEvent ResultEvent;
    ResultEvent.Type = ECortexStreamEventType::Result;
    ResultEvent.ResultText = TEXT("assistant reply");
    ResultEvent.DurationMs = 42;
    ResultEvent.NumTurns = 3;
    ResultEvent.TotalCostUsd = 1.25f;
    ResultEvent.SessionId = TEXT("session-turn");

    Session.HandleWorkerEvent(ResultEvent);

    TestTrue(TEXT("Turn complete delegate should fire"), bDelegateCalled);
    TestEqual(TEXT("Result text should be forwarded"), CapturedResult.ResultText, FString(TEXT("assistant reply")));
    TestEqual(TEXT("Session should return to idle after turn completion"), Session.GetStateForTest(), ECortexSessionState::Idle);
    return true;
}

bool FCortexCliSessionCancelTransitionsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("session-cancel");

    FCortexCliSession Session(Config);
    Session.SetStateForTest(ECortexSessionState::Processing);

    TestTrue(TEXT("Cancel should be accepted while processing"), Session.Cancel());
    // No process exists and CLI is not discovered, so cancel falls through:
    // Cancelling -> HandleProcessExited -> Respawning -> CLI not found -> Inactive
    TestEqual(TEXT("Cancel with no process should fall back to inactive"), Session.GetStateForTest(), ECortexSessionState::Inactive);
    return true;
}

bool FCortexCliSessionNewChatGeneratesFreshSessionIdTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("session-before");

    FCortexCliSession Session(Config);
    const FString OriginalSessionId = Session.GetSessionId();

    Session.NewChat();

    TestNotEqual(TEXT("New chat should generate a fresh session id"), Session.GetSessionId(), OriginalSessionId);
    TestEqual(TEXT("New chat should reset session state to inactive"), Session.GetStateForTest(), ECortexSessionState::Inactive);
    return true;
}

bool FCortexFrontendModuleGetOrCreateSessionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexFrontendModule& Module = FModuleManager::LoadModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));

    const TSharedPtr<FCortexCliSession> FirstSession = Module.GetOrCreateSession().Pin();
    const TSharedPtr<FCortexCliSession> SecondSession = Module.GetOrCreateSession().Pin();

    TestTrue(TEXT("Module should create a session"), FirstSession.IsValid());
    TestTrue(TEXT("Module should return the same session instance"), FirstSession == SecondSession);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionTurnIndexIncrementsTest,
    "Cortex.Frontend.Session.TurnIndexIncrements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliSessionTurnIndexIncrementsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-turn");
    FCortexCliSession Session(Config);

    Session.AddUserPromptEntry(TEXT("First message"));
    const TArray<TSharedPtr<FCortexChatEntry>>& Entries = Session.GetChatEntries();
    TestEqual(TEXT("First entry TurnIndex should be 1"), Entries.Last()->TurnIndex, 1);

    Session.AddUserPromptEntry(TEXT("Second message"));
    TestEqual(TEXT("Second entry TurnIndex should be 2"), Entries.Last()->TurnIndex, 2);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionTokenAccumulationTest,
    "Cortex.Frontend.Session.TokenAccumulation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliSessionTokenAccumulationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-tokens");
    FCortexCliSession Session(Config);

    FCortexStreamEvent Event;
    Event.Type = ECortexStreamEventType::TextContent;
    Event.Text = TEXT("Hello");
    Event.InputTokens = 1500;
    Event.OutputTokens = 200;
    Event.CacheReadTokens = 800;
    Event.CacheCreationTokens = 100;
    Session.HandleWorkerEvent(Event);

    TestEqual(TEXT("TotalInputTokens after first event"), Session.GetTotalInputTokens(), (int64)1500);
    TestEqual(TEXT("TotalOutputTokens after first event"), Session.GetTotalOutputTokens(), (int64)200);
    TestEqual(TEXT("ConversationContextTokens"), Session.GetConversationContextTokens(), (int64)1500);

    // NewChat should preserve session totals but reset conversation context
    Session.NewChat();
    TestEqual(TEXT("TotalInputTokens after NewChat"), Session.GetTotalInputTokens(), (int64)1500);
    TestEqual(TEXT("ConversationContextTokens after NewChat"), Session.GetConversationContextTokens(), (int64)0);

    // Second event should accumulate
    Session.HandleWorkerEvent(Event);
    TestEqual(TEXT("TotalInputTokens after second event"), Session.GetTotalInputTokens(), (int64)3000);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionModelInfoTest,
    "Cortex.Frontend.Session.ModelInfo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliSessionModelInfoTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-model");
    FCortexCliSession Session(Config);

    FCortexStreamEvent InitEvent;
    InitEvent.Type = ECortexStreamEventType::SessionInit;
    InitEvent.Model = TEXT("claude-sonnet-4-6");
    InitEvent.SessionId = TEXT("abc-123");
    Session.HandleWorkerEvent(InitEvent);

    TestEqual(TEXT("ModelId"), Session.GetModelId(), FString(TEXT("claude-sonnet-4-6")));
    TestEqual(TEXT("Provider"), Session.GetProvider(), FString(TEXT("Claude Code")));

    return true;
}
