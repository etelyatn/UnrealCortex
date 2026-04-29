#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexStateTreeCommandHandler.h"

namespace
{
    FCortexCommandRouter CreateStateTreeRouter()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("statetree"), TEXT("Cortex StateTree"), TEXT("1.0.0"),
            MakeShared<FCortexStateTreeCommandHandler>());
        return Router;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexStateTreeUnknownCommandTest,
    "Cortex.StateTree.CommandHandler.UnknownCommand",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeUnknownCommandTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateStateTreeRouter();
    FCortexCommandResult Result = Router.Execute(TEXT("statetree.does_not_exist"), MakeShared<FJsonObject>());

    TestFalse(TEXT("Unknown statetree command should fail"), Result.bSuccess);
    TestEqual(TEXT("Unknown command uses shared error code"), Result.ErrorCode, CortexErrorCodes::UnknownCommand);
    TestTrue(TEXT("Error message mentions statetree command"), Result.ErrorMessage.Contains(TEXT("Unknown statetree command")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexStateTreeSupportedCommandsTest,
    "Cortex.StateTree.CommandHandler.SupportedCommands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeSupportedCommandsTest::RunTest(const FString& Parameters)
{
    FCortexStateTreeCommandHandler Handler;
    const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();
    TestEqual(TEXT("PR1 surface should expose only get_status"), Commands.Num(), 1);

    TSet<FString> Names;
    for (const FCortexCommandInfo& Info : Commands)
    {
        Names.Add(Info.Name);
    }
    TestTrue(TEXT("Supported commands should include get_status"), Names.Contains(TEXT("get_status")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexStateTreeGetStatusTest,
    "Cortex.StateTree.CommandHandler.GetStatus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeGetStatusTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateStateTreeRouter();
    FCortexCommandResult Result = Router.Execute(TEXT("statetree.get_status"), MakeShared<FJsonObject>());

    TestTrue(TEXT("get_status should succeed"), Result.bSuccess);
    if (!Result.bSuccess || !Result.Data.IsValid())
    {
        return false;
    }

    FString Domain;
    TestTrue(TEXT("get_status data has domain field"), Result.Data->TryGetStringField(TEXT("domain"), Domain));
    TestEqual(TEXT("get_status domain is statetree"), Domain, FString(TEXT("statetree")));

    bool bRegistered = false;
    TestTrue(TEXT("get_status data has registered field"), Result.Data->TryGetBoolField(TEXT("registered"), bRegistered));
    TestTrue(TEXT("get_status reports registered=true"), bRegistered);
    return true;
}
