#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexQACommandHandler.h"

namespace
{
    FCortexCommandRouter CreateQARouter()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("qa"), TEXT("Cortex QA"), TEXT("1.0.0"),
            MakeShared<FCortexQACommandHandler>());
        return Router;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAUnknownCommandTest,
    "Cortex.QA.CommandHandler.UnknownCommand",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAUnknownCommandTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouter();
    FCortexCommandResult Result = Router.Execute(TEXT("qa.does_not_exist"), MakeShared<FJsonObject>());

    TestFalse(TEXT("Unknown qa command should fail"), Result.bSuccess);
    TestEqual(TEXT("Unknown command uses shared error code"), Result.ErrorCode, CortexErrorCodes::UnknownCommand);
    TestTrue(TEXT("Error message mentions qa command"), Result.ErrorMessage.Contains(TEXT("Unknown qa command")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASupportedCommandsTest,
    "Cortex.QA.CommandHandler.SupportedCommands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQASupportedCommandsTest::RunTest(const FString& Parameters)
{
    FCortexQACommandHandler Handler;
    const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();
    TestEqual(TEXT("Handler should expose full QA command set"), Commands.Num(), 11);

    TSet<FString> Names;
    for (const FCortexCommandInfo& Info : Commands)
    {
        Names.Add(Info.Name);
    }

    TestTrue(TEXT("Supported commands should include observe_state"), Names.Contains(TEXT("observe_state")));
    TestTrue(TEXT("Supported commands should include get_actor_state"), Names.Contains(TEXT("get_actor_state")));
    TestTrue(TEXT("Supported commands should include get_player_state"), Names.Contains(TEXT("get_player_state")));
    TestTrue(TEXT("Supported commands should include look_at"), Names.Contains(TEXT("look_at")));
    TestTrue(TEXT("Supported commands should include interact"), Names.Contains(TEXT("interact")));
    TestTrue(TEXT("Supported commands should include move_to"), Names.Contains(TEXT("move_to")));
    TestTrue(TEXT("Supported commands should include wait_for"), Names.Contains(TEXT("wait_for")));
    TestTrue(TEXT("Supported commands should include teleport_player"), Names.Contains(TEXT("teleport_player")));
    TestTrue(TEXT("Supported commands should include set_actor_property"), Names.Contains(TEXT("set_actor_property")));
    TestTrue(TEXT("Supported commands should include set_random_seed"), Names.Contains(TEXT("set_random_seed")));
    TestTrue(TEXT("Supported commands should include assert_state"), Names.Contains(TEXT("assert_state")));
    return true;
}
