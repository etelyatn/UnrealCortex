#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Providers/CortexCodexCliProvider.h"
#include "Providers/CortexCliProvider.h"
#include "Process/CortexCliDiscovery.h"

namespace
{
class FFakeCliProvider final : public ICortexCliProvider
{
public:
    FFakeCliProvider(FName InProviderId, FString InPath)
        : ProviderId(InProviderId)
        , Path(MoveTemp(InPath))
    {
    }

    virtual FName GetProviderId() const override
    {
        return ProviderId;
    }

    virtual const FCortexProviderDefinition& GetDefinition() const override
    {
        static const FCortexProviderDefinition DummyDefinition;
        return DummyDefinition;
    }

    virtual ECortexCliTransportMode GetTransportMode() const override
    {
        return ECortexCliTransportMode::PersistentSession;
    }

    virtual bool SupportsResume() const override
    {
        return true;
    }

    virtual FCortexCliInfo FindCli() const override
    {
        FCortexCliInfo Info;
        Info.ProviderId = ProviderId;
        Info.Path = Path;
        Info.bIsCmd = Path.EndsWith(TEXT(".cmd"));
        Info.bIsValid = true;
        return Info;
    }

    virtual FString BuildLaunchCommandLine(
        bool bResumeSession,
        ECortexAccessMode AccessMode,
        const FCortexSessionConfig& SessionConfig) const override
    {
        return FString::Printf(
            TEXT("%s|%s|%s|%d|%d"),
            *SessionConfig.WorkingDirectory,
            *SessionConfig.SessionId,
            *SessionConfig.McpConfigPath,
            static_cast<int32>(AccessMode),
            bResumeSession ? 1 : 0);
    }

    virtual FString BuildAuthCommand() const override
    {
        return FString::Printf(TEXT("%s login"), *ProviderId.ToString());
    }

    virtual void ConsumeStreamChunk(
        const FString& RawChunk,
        FString& InOutChunkBuffer,
        TArray<FCortexStreamEvent>& OutEvents) const override
    {
        InOutChunkBuffer += RawChunk;
        FCortexStreamEvent Event;
        Event.RawJson = RawChunk;
        Event.SessionId = ProviderId.ToString();
        OutEvents.Add(MoveTemp(Event));
    }

private:
    FName ProviderId;
    FString Path;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliDiscoveryFindClaudeTest, "Cortex.Frontend.CliDiscovery.FindClaude", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliDiscoveryFindClaudeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexCliDiscovery::ClearCache();
    FCortexCliDiscovery::ClearProviderOverridesForTest();
    ON_SCOPE_EXIT
    {
        FCortexCliDiscovery::ClearProviderOverridesForTest();
        FCortexCliDiscovery::ClearCache();
    };

    const FCortexCliInfo Info = FCortexCliDiscovery::FindClaude();
    if (Info.bIsValid)
    {
        TestFalse(TEXT("Path should not be empty when valid"), Info.Path.IsEmpty());
        AddInfo(FString::Printf(TEXT("Found Claude at: %s (isCmd=%d)"), *Info.Path, Info.bIsCmd));
    }
    else
    {
        AddInfo(TEXT("Claude CLI not found - search completed without crash"));
    }
    const FCortexCliInfo Info2 = FCortexCliDiscovery::FindClaude();
    TestEqual(TEXT("Second call should return same path"), Info.Path, Info2.Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliDiscoveryFindCodexTest, "Cortex.Frontend.CliDiscovery.FindCodex", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliDiscoveryFindCodexTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexCliDiscovery::ClearCache();
    FCortexCliDiscovery::ClearProviderOverridesForTest();
    ON_SCOPE_EXIT
    {
        FCortexCliDiscovery::ClearProviderOverridesForTest();
        FCortexCliDiscovery::ClearCache();
    };

    const FCortexCliInfo Info = FCortexCliDiscovery::Find(FName(TEXT("codex")));
    if (!Info.bIsValid)
    {
        AddInfo(TEXT("Codex CLI not found - search completed without crash"));
        return true;
    }

    TestEqual(TEXT("Provider id should be codex"), Info.ProviderId, FName(TEXT("codex")));
    TestFalse(TEXT("Path should not be empty when valid"), Info.Path.IsEmpty());
    const FCortexCliInfo Info2 = FCortexCliDiscovery::Find(FName(TEXT("codex")));
    TestEqual(TEXT("Second codex call should return same path"), Info.Path, Info2.Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliDiscoveryProviderScopedCacheTest, "Cortex.Frontend.CliDiscovery.ProviderScopedCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliDiscoveryProviderScopedCacheTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexCliDiscovery::ClearCache();
    FCortexCliDiscovery::ClearProviderOverridesForTest();
    ON_SCOPE_EXIT
    {
        FCortexCliDiscovery::ClearProviderOverridesForTest();
        FCortexCliDiscovery::ClearCache();
    };

    FCortexCliDiscovery::SetProviderOverrideForTest(FName(TEXT("claude_code")), MakeShared<FFakeCliProvider>(FName(TEXT("claude_code")), TEXT("C:/Temp/claude-a.exe")));
    FCortexCliDiscovery::SetProviderOverrideForTest(FName(TEXT("codex")), MakeShared<FFakeCliProvider>(FName(TEXT("codex")), TEXT("C:/Temp/codex-a.exe")));

    const FCortexCliInfo ClaudeInfo = FCortexCliDiscovery::Find(FName(TEXT("claude_code")));
    const FCortexCliInfo CodexInfo = FCortexCliDiscovery::Find(FName(TEXT("codex")));

    TestEqual(TEXT("Claude cache entry should keep Claude path"), ClaudeInfo.Path, FString(TEXT("C:/Temp/claude-a.exe")));
    TestEqual(TEXT("Codex cache entry should keep Codex path"), CodexInfo.Path, FString(TEXT("C:/Temp/codex-a.exe")));

    FCortexCliDiscovery::SetProviderOverrideForTest(FName(TEXT("codex")), MakeShared<FFakeCliProvider>(FName(TEXT("codex")), TEXT("C:/Temp/codex-b.exe")));
    const FCortexCliInfo CachedCodexInfo = FCortexCliDiscovery::Find(FName(TEXT("codex")));
    TestEqual(TEXT("Codex cache should be provider-scoped"), CachedCodexInfo.Path, FString(TEXT("C:/Temp/codex-a.exe")));
    TestTrue(TEXT("Claude and Codex cached results should differ"), ClaudeInfo.Path != CachedCodexInfo.Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliDiscoveryCodexLaunchCommandTest, "Cortex.Frontend.CliDiscovery.CodexLaunchCommandIncludesResolvedOptions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliDiscoveryCodexLaunchCommandTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig SessionConfig;
    SessionConfig.SessionId = TEXT("session-123");
    SessionConfig.WorkingDirectory = TEXT("D:/UnrealProjects/CortexSandbox");
    SessionConfig.McpConfigPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));
    SessionConfig.ModelId = TEXT("gpt-5.4");
    SessionConfig.EffortLevel = ECortexEffortLevel::Maximum;
    SessionConfig.bSkipPermissions = true;

    const FCortexCodexCliProvider Provider;
    TestTrue(TEXT("Codex provider should report resume support"), Provider.SupportsResume());

    const FString CommandLine = Provider.BuildLaunchCommandLine(true, ECortexAccessMode::Guided, SessionConfig);
    AddInfo(FString::Printf(TEXT("Codex resume command: %s"), *CommandLine));

    TestTrue(TEXT("Codex launch should use exec resume json when resuming"), CommandLine.Contains(TEXT("exec resume --json")));
    TestTrue(TEXT("Codex resume should include the session id"), CommandLine.Contains(TEXT("session-123")));
    TestFalse(
        TEXT("Codex resume should not include the working directory"),
        CommandLine.Contains(FString::Printf(TEXT("-C \"%s\""), *SessionConfig.WorkingDirectory)));
    TestTrue(TEXT("Codex launch should include model flag"), CommandLine.Contains(TEXT("-m \"gpt-5.4\"")));
    TestTrue(TEXT("Codex launch should include reasoning effort"), CommandLine.Contains(TEXT("-c model_reasoning_effort=maximum")));
    TestTrue(TEXT("Codex launch should include MCP command override"), CommandLine.Contains(TEXT("mcp_servers.cortex_mcp.command")));
    TestTrue(TEXT("Codex launch should include bypass approvals flag when skip permissions is set"), CommandLine.Contains(TEXT("--dangerously-bypass-approvals-and-sandbox")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliDiscoveryCodexNormalizationTest, "Cortex.Frontend.CliDiscovery.CodexNormalizesObservedJsonlEvents", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliDiscoveryCodexNormalizationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FCortexCodexCliProvider Provider;
    FString ChunkBuffer;
    TArray<FCortexStreamEvent> Events;

    const FString RawJsonl = TEXT(R"({"type":"thread.started","thread":{"id":"thread-123"}}
{"type":"turn.started"}
{"type":"item.started","item":{"id":"item-123","type":"command_execution","command":"ls","args":["-la"]}}
{"type":"item.completed","item":{"id":"item-123","type":"command_execution","output":"done"}}
{"type":"item.completed","item":{"id":"msg-1","type":"agent_message","text":"OK"}}
{"type":"turn.completed","usage":{"input_tokens":12,"output_tokens":34}}
)");

    Provider.ConsumeStreamChunk(RawJsonl, ChunkBuffer, Events);

    TestTrue(TEXT("Codex normalization should emit session init"), Events.ContainsByPredicate([](const FCortexStreamEvent& Event)
    {
        return Event.Type == ECortexStreamEventType::SessionInit && Event.SessionId == TEXT("thread-123");
    }));
    TestTrue(TEXT("Codex normalization should emit command execution tool use"), Events.ContainsByPredicate([](const FCortexStreamEvent& Event)
    {
        return Event.Type == ECortexStreamEventType::ToolUse && Event.ToolName == TEXT("command_execution") && Event.ToolCallId == TEXT("item-123");
    }));
    TestTrue(TEXT("Codex normalization should emit command execution result"), Events.ContainsByPredicate([](const FCortexStreamEvent& Event)
    {
        return Event.Type == ECortexStreamEventType::ToolResult && Event.ToolCallId == TEXT("item-123") && Event.ToolResultContent == TEXT("done");
    }));
    TestTrue(TEXT("Codex normalization should emit agent text"), Events.ContainsByPredicate([](const FCortexStreamEvent& Event)
    {
        return Event.Type == ECortexStreamEventType::TextContent && Event.Text == TEXT("OK");
    }));
    TestTrue(TEXT("Codex normalization should emit turn result"), Events.ContainsByPredicate([](const FCortexStreamEvent& Event)
    {
        return Event.Type == ECortexStreamEventType::Result && Event.InputTokens == 12 && Event.OutputTokens == 34;
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliDiscoveryCodexFailureNormalizationTest, "Cortex.Frontend.CliDiscovery.CodexNormalizesFailureEvents", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliDiscoveryCodexFailureNormalizationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FCortexCodexCliProvider Provider;
    FString ChunkBuffer;
    TArray<FCortexStreamEvent> Events;

    const FString RawJsonl = TEXT(R"({"type":"turn.failed","message":"codex stopped unexpectedly","usage":{"input_tokens":5,"output_tokens":1}}
{"type":"error","error":{"type":"transport_error","message":"connection reset"}}
)");

    Provider.ConsumeStreamChunk(RawJsonl, ChunkBuffer, Events);

    TestTrue(TEXT("Codex failure normalization should emit a system error for turn.failed"), Events.ContainsByPredicate([](const FCortexStreamEvent& Event)
    {
        return Event.Type == ECortexStreamEventType::SystemError && Event.bIsError && Event.Text == TEXT("codex stopped unexpectedly");
    }));
    TestTrue(TEXT("Codex failure normalization should emit a system error for error events"), Events.ContainsByPredicate([](const FCortexStreamEvent& Event)
    {
        return Event.Type == ECortexStreamEventType::SystemError && Event.bIsError && Event.Text == TEXT("connection reset");
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliDiscoveryCodexUnknownItemTest, "Cortex.Frontend.CliDiscovery.CodexIgnoresUnknownItemTypesCleanly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliDiscoveryCodexUnknownItemTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FCortexCodexCliProvider Provider;
    FString ChunkBuffer;
    TArray<FCortexStreamEvent> Events;

    const FString RawJsonl = TEXT(R"({"type":"item.started","item":{"id":"item-999","type":"mcp_tool_use","name":"cortex_mcp","input":{"query":"status"}}}
{"type":"item.completed","item":{"id":"item-999","type":"mcp_tool_use","output":"done"}}
{"type":"turn.completed","usage":{"input_tokens":7,"output_tokens":2}}
)");

    Provider.ConsumeStreamChunk(RawJsonl, ChunkBuffer, Events);

    TestEqual(TEXT("Unknown Codex item types should not create extra events"), Events.Num(), 1);
    TestTrue(TEXT("Turn.completed should still produce a result event"), Events.ContainsByPredicate([](const FCortexStreamEvent& Event)
    {
        return Event.Type == ECortexStreamEventType::Result && Event.InputTokens == 7 && Event.OutputTokens == 2;
    }));
    return true;
}
