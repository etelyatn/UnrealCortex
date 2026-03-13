#include "Misc/AutomationTest.h"
#include "Process/CortexCliRunner.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliRunnerBuildCommandLineTest, "Cortex.Frontend.CliRunner.BuildCommandLine", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliRunnerConcurrencyGuardTest, "Cortex.Frontend.CliRunner.ConcurrencyGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliRunnerAllowedToolsTest, "Cortex.Frontend.CliRunner.AllowedToolsModes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliRunnerBuildCommandLineTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexCliRunner Runner;
    FCortexChatRequest Request;
    Request.SessionId = TEXT("session-123");
    Request.bIsFirstMessage = false;
    Request.AccessMode = ECortexAccessMode::ReadOnly;
    Request.bSkipPermissions = true;
    Request.McpConfigPath = TEXT("D:/UnrealProjects/CortexSandbox/.mcp.json");

    const FString CommandLine = Runner.BuildCommandLine(Request);

    TestTrue(TEXT("Session id should be included"), CommandLine.Contains(TEXT("--session-id \"session-123\"")));
    TestTrue(TEXT("Resume flag should be included for non-first messages"), CommandLine.Contains(TEXT("--resume")));
    TestTrue(TEXT("Skip permissions flag should be included"), CommandLine.Contains(TEXT("--dangerously-skip-permissions")));
    TestTrue(TEXT("Read-only allowed tools should be present"), CommandLine.Contains(TEXT("mcp__cortex_mcp__get_*")));
    TestTrue(TEXT("MCP config path should be included"), CommandLine.Contains(TEXT("--mcp-config \"D:/UnrealProjects/CortexSandbox/.mcp.json\"")));
    return true;
}

bool FCortexCliRunnerConcurrencyGuardTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexCliRunner Runner;
    TestFalse(TEXT("Should not be executing after construction"), Runner.IsExecuting());
    return true;
}

bool FCortexCliRunnerAllowedToolsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexCliRunner Runner;
    const FString ReadOnlyTools = Runner.BuildAllowedToolsArg(ECortexAccessMode::ReadOnly);
    const FString GuidedTools = Runner.BuildAllowedToolsArg(ECortexAccessMode::Guided);
    const FString FullAccessTools = Runner.BuildAllowedToolsArg(ECortexAccessMode::FullAccess);

    TestTrue(TEXT("Read-only tools should include list access"), ReadOnlyTools.Contains(TEXT("mcp__cortex_mcp__list_*")));
    TestFalse(TEXT("Read-only tools should not include spawn access"), ReadOnlyTools.Contains(TEXT("mcp__cortex_mcp__spawn_*")));
    TestTrue(TEXT("Guided tools should include spawn access"), GuidedTools.Contains(TEXT("mcp__cortex_mcp__spawn_*")));
    TestFalse(TEXT("Guided tools should not include destructive graph ops"), GuidedTools.Contains(TEXT("mcp__cortex_mcp__graph_remove_*")));
    TestFalse(TEXT("Guided tools should not include graph disconnect"), GuidedTools.Contains(TEXT("mcp__cortex_mcp__graph_disconnect")));
    TestTrue(TEXT("Guided tools should include graph add ops"), GuidedTools.Contains(TEXT("mcp__cortex_mcp__graph_add_*")));
    TestTrue(TEXT("Full access should have no restrictions"), FullAccessTools.IsEmpty());
    return true;
}
