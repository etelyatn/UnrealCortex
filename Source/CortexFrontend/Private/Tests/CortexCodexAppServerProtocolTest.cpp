#include "Misc/AutomationTest.h"
#include "Providers/CortexCodexAppServerProtocol.h"
#include "Session/CortexSessionTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodexAppServerBuildInitializeTest,
    "Cortex.Frontend.CodexAppServer.Protocol.BuildInitialize",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodexAppServerBuildThreadStartTest,
    "Cortex.Frontend.CodexAppServer.Protocol.BuildThreadStart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodexAppServerBuildTurnStartTest,
    "Cortex.Frontend.CodexAppServer.Protocol.BuildTurnStart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodexAppServerParseNotificationsTest,
    "Cortex.Frontend.CodexAppServer.Protocol.ParseNotifications",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodexAppServerParseToolAndUsageNotificationsTest,
    "Cortex.Frontend.CodexAppServer.Protocol.ParseToolAndUsageNotifications",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodexAppServerBuildInitializeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Json = FCortexCodexAppServerProtocol::BuildInitializeRequest(7);

    TestTrue(TEXT("Initialize request should use JSON-RPC initialize method"),
        Json.Contains(TEXT("\"method\":\"initialize\"")));
    TestTrue(TEXT("Initialize request should include request id"),
        Json.Contains(TEXT("\"id\":7")));
    TestTrue(TEXT("Initialize request should identify Cortex Frontend"),
        Json.Contains(TEXT("\"name\":\"cortex_frontend\"")));
    TestTrue(TEXT("Initialize request should opt in to experimental API"),
        Json.Contains(TEXT("\"experimentalApi\":true")));

    const FString Notification = FCortexCodexAppServerProtocol::BuildInitializedNotification();
    TestEqual(TEXT("Initialized notification should be id-less"),
        Notification,
        FString(TEXT("{\"method\":\"initialized\",\"params\":{}}\n")));

    return true;
}

bool FCortexCodexAppServerBuildThreadStartTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.WorkingDirectory = TEXT("D:/UnrealProjects/CortexSandboxMirror");
    Config.McpConfigPath = TEXT("D:/UnrealProjects/CortexSandboxMirror/.mcp.json");
    Config.SystemPrompt = TEXT("Cortex app-server base instructions.");
    Config.ResolvedOptions.ModelId = TEXT("gpt-5.4");
    Config.LaunchOptions.AccessMode = ECortexAccessMode::Guided;
    Config.LaunchOptions.bSkipPermissions = false;
    Config.LaunchOptions.CustomDirective = TEXT("Prefer concise answers.");

    const FString Json = FCortexCodexAppServerProtocol::BuildThreadStartRequest(
        9,
        Config,
        ECortexAccessMode::Guided);

    TestTrue(TEXT("Thread start should use thread/start method"),
        Json.Contains(TEXT("\"method\":\"thread/start\"")));
    TestTrue(TEXT("Thread start should include model"),
        Json.Contains(TEXT("\"model\":\"gpt-5.4\"")));
    TestTrue(TEXT("Thread start should include cwd"),
        Json.Contains(TEXT("\"cwd\":\"D:/UnrealProjects/CortexSandboxMirror\"")));
    TestTrue(TEXT("Guided access should map to workspace-write sandbox"),
        Json.Contains(TEXT("\"sandbox\":\"workspace-write\"")));
    TestTrue(TEXT("Thread start should use never approval policy for frontend controlled sends"),
        Json.Contains(TEXT("\"approvalPolicy\":\"never\"")));
    TestTrue(TEXT("Thread start should include base instructions"),
        Json.Contains(TEXT("Cortex app-server base instructions.")));
    TestTrue(TEXT("Thread start should pass MCP config through structured config overrides"),
        Json.Contains(TEXT("mcp_servers")));
    TestFalse(TEXT("Thread start config should not contain CLI -c arguments"),
        Json.Contains(TEXT("\"-c\"")));

    return true;
}

bool FCortexCodexAppServerBuildTurnStartTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.ResolvedOptions.ModelId = TEXT("gpt-5.4");
    Config.ResolvedOptions.EffortLevel = ECortexEffortLevel::High;

    const FString Json = FCortexCodexAppServerProtocol::BuildTurnStartRequest(
        10,
        TEXT("thread-123"),
        TEXT("Inspect the selected Blueprint."),
        Config,
        ECortexAccessMode::ReadOnly);

    TestTrue(TEXT("Turn start should use turn/start method"),
        Json.Contains(TEXT("\"method\":\"turn/start\"")));
    TestTrue(TEXT("Turn start should include thread id"),
        Json.Contains(TEXT("\"threadId\":\"thread-123\"")));
    TestTrue(TEXT("Turn input should be a text item"),
        Json.Contains(TEXT("\"type\":\"text\"")));
    TestTrue(TEXT("Turn input should include text_elements"),
        Json.Contains(TEXT("\"text_elements\":[]")));
    TestTrue(TEXT("Read-only access should map to a readOnly sandbox policy"),
        Json.Contains(TEXT("\"type\":\"readOnly\"")));
    TestTrue(TEXT("High effort should map to high"),
        Json.Contains(TEXT("\"effort\":\"high\"")));

    return true;
}

bool FCortexCodexAppServerParseNotificationsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexCodexAppServerProtocolState State;
    TArray<FCortexStreamEvent> Events;

    FCortexCodexAppServerProtocol::ParseLine(
        TEXT("{\"id\":1,\"result\":{\"thread\":{\"id\":\"thread-abc\",\"sessionId\":\"session-abc\"},\"model\":\"gpt-5.4\",\"modelProvider\":\"openai\"}}"),
        State,
        Events);
    TestEqual(TEXT("Thread id should be captured from thread/start response"),
        State.ThreadId,
        FString(TEXT("thread-abc")));
    TestEqual(TEXT("Thread start response should emit SessionInit"),
        Events.Num(),
        1);
    TestEqual(TEXT("Session init event should carry thread id"),
        Events[0].SessionId,
        FString(TEXT("thread-abc")));

    Events.Reset();
    FCortexCodexAppServerProtocol::ParseLine(
        TEXT("{\"method\":\"item/agentMessage/delta\",\"params\":{\"threadId\":\"thread-abc\",\"turnId\":\"turn-1\",\"itemId\":\"item-1\",\"delta\":\"Hello\"}}"),
        State,
        Events);
    TestEqual(TEXT("Agent message delta should emit one stream event"),
        Events.Num(),
        1);
    TestEqual(TEXT("Agent message delta should map to ContentBlockDelta"),
        Events[0].Type,
        ECortexStreamEventType::ContentBlockDelta);
    TestEqual(TEXT("Agent message delta text should be preserved"),
        Events[0].Text,
        FString(TEXT("Hello")));

    Events.Reset();
    FCortexCodexAppServerProtocol::ParseLine(
        TEXT("{\"method\":\"turn/completed\",\"params\":{\"threadId\":\"thread-abc\",\"turn\":{\"id\":\"turn-1\",\"items\":[],\"itemsView\":\"full\",\"status\":\"completed\",\"error\":null,\"startedAt\":1,\"completedAt\":2,\"durationMs\":1234}}}"),
        State,
        Events);
    TestEqual(TEXT("Turn completed should emit one result event"),
        Events.Num(),
        1);
    TestEqual(TEXT("Turn completed should map to Result"),
        Events[0].Type,
        ECortexStreamEventType::Result);
    TestEqual(TEXT("Result text should use accumulated assistant text"),
        Events[0].ResultText,
        FString(TEXT("Hello")));
    TestEqual(TEXT("Duration should be copied from turn"),
        Events[0].DurationMs,
        1234);

    return true;
}

bool FCortexCodexAppServerParseToolAndUsageNotificationsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexCodexAppServerProtocolState State;
    State.ThreadId = TEXT("thread-abc");
    TArray<FCortexStreamEvent> Events;

    FCortexCodexAppServerProtocol::ParseLine(
        TEXT("{\"method\":\"item/started\",\"params\":{\"threadId\":\"thread-abc\",\"turnId\":\"turn-1\",\"startedAtMs\":1,\"item\":{\"type\":\"commandExecution\",\"id\":\"cmd-1\",\"command\":\"rg Cortex\",\"cwd\":\"D:/repo\",\"processId\":null,\"source\":\"agent\",\"status\":\"inProgress\",\"commandActions\":[],\"aggregatedOutput\":null,\"exitCode\":null,\"durationMs\":null}}}"),
        State,
        Events);
    TestEqual(TEXT("commandExecution item start should emit one event"), Events.Num(), 1);
    TestEqual(TEXT("commandExecution item start should map to ToolUse"), Events[0].Type, ECortexStreamEventType::ToolUse);
    TestEqual(TEXT("commandExecution should use the command text as tool name"), Events[0].ToolName, FString(TEXT("rg Cortex")));

    Events.Reset();
    FCortexCodexAppServerProtocol::ParseLine(
        TEXT("{\"method\":\"item/completed\",\"params\":{\"threadId\":\"thread-abc\",\"turnId\":\"turn-1\",\"completedAtMs\":2,\"item\":{\"type\":\"mcpToolCall\",\"id\":\"tool-1\",\"server\":\"cortex_mcp\",\"tool\":\"data.get_data_catalog\",\"status\":\"completed\",\"arguments\":{\"limit\":5},\"pluginId\":null,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]},\"error\":null,\"durationMs\":17}}}"),
        State,
        Events);
    TestEqual(TEXT("mcpToolCall item completion should emit one event"), Events.Num(), 1);
    TestEqual(TEXT("mcpToolCall item completion should map to ToolResult"), Events[0].Type, ECortexStreamEventType::ToolResult);
    TestEqual(TEXT("mcpToolCall tool name should use the tool field"), Events[0].ToolName, FString(TEXT("data.get_data_catalog")));
    TestTrue(TEXT("mcpToolCall result content should be serialized for display"), Events[0].ToolResultContent.Contains(TEXT("ok")));

    Events.Reset();
    FCortexCodexAppServerProtocol::ParseLine(
        TEXT("{\"method\":\"thread/tokenUsage/updated\",\"params\":{\"threadId\":\"thread-abc\",\"turnId\":\"turn-1\",\"tokenUsage\":{\"total\":{\"totalTokens\":42,\"inputTokens\":30,\"cachedInputTokens\":11,\"outputTokens\":12,\"reasoningOutputTokens\":4},\"last\":{\"totalTokens\":20,\"inputTokens\":15,\"cachedInputTokens\":5,\"outputTokens\":5,\"reasoningOutputTokens\":1},\"modelContextWindow\":400000}}}"),
        State,
        Events);
    TestEqual(TEXT("Token usage update should emit one accounting event"), Events.Num(), 1);
    TestEqual(TEXT("Total input tokens should come from tokenUsage.total.inputTokens"), Events[0].InputTokens, static_cast<int64>(30));
    TestEqual(TEXT("Cached input tokens should come from tokenUsage.total.cachedInputTokens"), Events[0].CacheReadTokens, static_cast<int64>(11));
    TestEqual(TEXT("Output tokens should come from tokenUsage.total.outputTokens"), Events[0].OutputTokens, static_cast<int64>(12));

    return true;
}
