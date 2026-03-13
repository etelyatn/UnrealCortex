#include "Misc/AutomationTest.h"
#include "Process/CortexCliRunner.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliRunnerConcurrencyGuardTest, "Cortex.Frontend.CliRunner.ConcurrencyGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliRunnerAllowedToolsTest, "Cortex.Frontend.CliRunner.AllowedToolsModes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
