#include "Misc/AutomationTest.h"
#include "Session/CortexCliSession.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionBuildInitialLaunchArgsTest, "Cortex.Frontend.CliSession.BuildInitialLaunchArgs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionBuildResumeLaunchArgsTest, "Cortex.Frontend.CliSession.BuildResumeLaunchArgs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliSessionBuildPromptEnvelopeTest, "Cortex.Frontend.CliSession.BuildPromptEnvelope", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
