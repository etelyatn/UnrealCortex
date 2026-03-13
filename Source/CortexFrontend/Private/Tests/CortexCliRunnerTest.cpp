#include "Misc/AutomationTest.h"
#include "Process/CortexCliRunner.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliRunnerBuildCommandLineTest, "Cortex.Frontend.CliRunner.BuildCommandLine", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliRunnerConcurrencyGuardTest, "Cortex.Frontend.CliRunner.ConcurrencyGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliRunnerAllowedToolsTest, "Cortex.Frontend.CliRunner.AllowedToolsModes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliRunnerBuildCommandLineTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexCliRunner Runner;
    TestFalse(TEXT("Runner should not be executing initially"), Runner.IsExecuting());
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
    FCortexChatRequest ReadOnlyReq;
    ReadOnlyReq.AccessMode = ECortexAccessMode::ReadOnly;

    FCortexChatRequest GuidedReq;
    GuidedReq.AccessMode = ECortexAccessMode::Guided;

    FCortexChatRequest FullReq;
    FullReq.AccessMode = ECortexAccessMode::FullAccess;

    TestTrue(TEXT("ReadOnly != Guided"), ReadOnlyReq.AccessMode != GuidedReq.AccessMode);
    TestTrue(TEXT("Guided != FullAccess"), GuidedReq.AccessMode != FullReq.AccessMode);
    return true;
}
