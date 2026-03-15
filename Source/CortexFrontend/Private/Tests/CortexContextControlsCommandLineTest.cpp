#include "Misc/AutomationTest.h"
#include "CortexFrontendSettings.h"
#include "Session/CortexCliSession.h"

// -- Effort flag tests --

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCmdLineEffortDefaultTest,
    "Cortex.Frontend.ContextControls.CommandLine.EffortDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCmdLineEffortDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexEffortLevel Orig = Settings.GetEffortLevel();
    Settings.SetEffortLevel(ECortexEffortLevel::Default);

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-effort");
    FCortexCliSession Session(Config);
    const FString CmdLine = Session.BuildLaunchCommandLine(false, ECortexAccessMode::Guided);

    TestFalse(TEXT("No --effort flag when Default"), CmdLine.Contains(TEXT("--effort")));

    Settings.SetEffortLevel(Orig);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCmdLineEffortMediumTest,
    "Cortex.Frontend.ContextControls.CommandLine.EffortMedium",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCmdLineEffortMediumTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexEffortLevel Orig = Settings.GetEffortLevel();
    Settings.SetEffortLevel(ECortexEffortLevel::Medium);

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-effort-med");
    FCortexCliSession Session(Config);
    const FString CmdLine = Session.BuildLaunchCommandLine(false, ECortexAccessMode::Guided);

    TestTrue(TEXT("Has --effort medium"), CmdLine.Contains(TEXT("--effort \"medium\"")));

    Settings.SetEffortLevel(Orig);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCmdLineWorkflowDirectTest,
    "Cortex.Frontend.ContextControls.CommandLine.WorkflowDirect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCmdLineWorkflowDirectTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexWorkflowMode Orig = Settings.GetWorkflowMode();
    Settings.SetWorkflowMode(ECortexWorkflowMode::Direct);

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-workflow");
    FCortexCliSession Session(Config);
    const FString CmdLine = Session.BuildLaunchCommandLine(false, ECortexAccessMode::Guided);

    TestTrue(TEXT("Has --disable-slash-commands"), CmdLine.Contains(TEXT("--disable-slash-commands")));
    TestTrue(TEXT("System prompt contains Direct hint"), CmdLine.Contains(TEXT("Workflow mode: Direct")));

    Settings.SetWorkflowMode(Orig);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCmdLineWorkflowThoroughTest,
    "Cortex.Frontend.ContextControls.CommandLine.WorkflowThorough",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCmdLineWorkflowThoroughTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexWorkflowMode Orig = Settings.GetWorkflowMode();
    Settings.SetWorkflowMode(ECortexWorkflowMode::Thorough);

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-workflow-thorough");
    FCortexCliSession Session(Config);
    const FString CmdLine = Session.BuildLaunchCommandLine(false, ECortexAccessMode::Guided);

    TestFalse(TEXT("No --disable-slash-commands"), CmdLine.Contains(TEXT("--disable-slash-commands")));
    TestFalse(TEXT("No Direct hint in prompt"), CmdLine.Contains(TEXT("Workflow mode: Direct")));

    Settings.SetWorkflowMode(Orig);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCmdLineProjectContextOffTest,
    "Cortex.Frontend.ContextControls.CommandLine.ProjectContextOff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCmdLineProjectContextOffTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const bool bOrig = Settings.GetProjectContext();
    Settings.SetProjectContext(false);

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-context-off");
    FCortexCliSession Session(Config);
    const FString CmdLine = Session.BuildLaunchCommandLine(false, ECortexAccessMode::Guided);

    TestTrue(TEXT("Has --setting-sources"), CmdLine.Contains(TEXT("--setting-sources \"user,local\"")));

    Settings.SetProjectContext(bOrig);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCmdLineProjectContextOnTest,
    "Cortex.Frontend.ContextControls.CommandLine.ProjectContextOn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCmdLineProjectContextOnTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const bool bOrig = Settings.GetProjectContext();
    Settings.SetProjectContext(true);

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-context-on");
    FCortexCliSession Session(Config);
    const FString CmdLine = Session.BuildLaunchCommandLine(false, ECortexAccessMode::Guided);

    TestFalse(TEXT("No --setting-sources when on"), CmdLine.Contains(TEXT("--setting-sources")));

    Settings.SetProjectContext(bOrig);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCmdLineDirectiveTest,
    "Cortex.Frontend.ContextControls.CommandLine.CustomDirective",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCmdLineDirectiveTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const FString Orig = Settings.GetCustomDirective();
    Settings.SetCustomDirective(TEXT("Focus on Blueprints"));

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-directive");
    FCortexCliSession Session(Config);
    const FString CmdLine = Session.BuildLaunchCommandLine(false, ECortexAccessMode::Guided);

    TestTrue(TEXT("System prompt contains directive"), CmdLine.Contains(TEXT("Focus on Blueprints")));

    Settings.SetCustomDirective(Orig);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCmdLineDirectiveEmptyTest,
    "Cortex.Frontend.ContextControls.CommandLine.DirectiveEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCmdLineDirectiveEmptyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const FString Orig = Settings.GetCustomDirective();
    Settings.SetCustomDirective(TEXT(""));

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-no-directive");
    FCortexCliSession Session(Config);
    const FString CmdLine = Session.BuildLaunchCommandLine(false, ECortexAccessMode::Guided);

    TestTrue(TEXT("Has base system prompt"), CmdLine.Contains(TEXT("--append-system-prompt")));

    Settings.SetCustomDirective(Orig);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCmdLineDirectiveSanitizationTest,
    "Cortex.Frontend.ContextControls.CommandLine.DirectiveSanitization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCmdLineDirectiveSanitizationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const FString Orig = Settings.GetCustomDirective();
    Settings.SetCustomDirective(TEXT("Focus\non$(rm -rf)\tBlueprints`echo`%PATH%|cat&>out<in^x"));

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("test-sanitize");
    FCortexCliSession Session(Config);
    const FString CmdLine = Session.BuildLaunchCommandLine(false, ECortexAccessMode::Guided);

    TestFalse(TEXT("No $("), CmdLine.Contains(TEXT("$(")));
    TestFalse(TEXT("No backtick"), CmdLine.Contains(TEXT("`")));
    TestFalse(TEXT("No percent"), CmdLine.Contains(TEXT("%")));
    TestFalse(TEXT("No pipe"), CmdLine.Contains(TEXT("|cat")));
    TestFalse(TEXT("No ampersand"), CmdLine.Contains(TEXT("&>")));
    TestFalse(TEXT("No caret"), CmdLine.Contains(TEXT("^x")));
    TestTrue(TEXT("Focus survives"), CmdLine.Contains(TEXT("Focus")));
    TestTrue(TEXT("Blueprints survives"), CmdLine.Contains(TEXT("Blueprints")));

    Settings.SetCustomDirective(Orig);
    Settings.ClearPendingChanges();
    return true;
}
