#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"

namespace
{
    FCortexCommandRouter CreateLevelRouterStreaming()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.0"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelGetInfoTest,
    "Cortex.Level.Streaming.GetInfo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelGetInfoTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterStreaming();
    FCortexCommandResult Result = Router.Execute(TEXT("level.get_info"), MakeShared<FJsonObject>());
    TestTrue(TEXT("get_info should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        TestTrue(TEXT("level_name should exist"), Result.Data->HasField(TEXT("level_name")));
        TestTrue(TEXT("level_path should exist"), Result.Data->HasField(TEXT("level_path")));
        TestTrue(TEXT("world_type should exist"), Result.Data->HasField(TEXT("world_type")));
        TestTrue(TEXT("actor_count should exist"), Result.Data->HasField(TEXT("actor_count")));
        TestTrue(TEXT("is_world_partition should exist"), Result.Data->HasField(TEXT("is_world_partition")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelListSublevelsTest,
    "Cortex.Level.Streaming.ListSublevels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListSublevelsTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterStreaming();
    FCortexCommandResult Result = Router.Execute(TEXT("level.list_sublevels"), MakeShared<FJsonObject>());
    TestTrue(TEXT("list_sublevels should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        TestTrue(TEXT("sublevels array should exist"), Result.Data->HasField(TEXT("sublevels")));
        TestTrue(TEXT("count should exist"), Result.Data->HasField(TEXT("count")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSaveLevelTest,
    "Cortex.Level.Streaming.SaveLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSaveLevelTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterStreaming();
    FCortexCommandResult Result = Router.Execute(TEXT("level.save_level"), MakeShared<FJsonObject>());
    TestTrue(TEXT("save_level should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        bool bSaved = false;
        Result.Data->TryGetBoolField(TEXT("saved"), bSaved);
        TestTrue(TEXT("saved should be true"), bSaved);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSaveAllTest,
    "Cortex.Level.Streaming.SaveAll",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSaveAllTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterStreaming();
    FCortexCommandResult Result = Router.Execute(TEXT("level.save_all"), MakeShared<FJsonObject>());
    TestTrue(TEXT("save_all should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        TestTrue(TEXT("saved field should exist"), Result.Data->HasField(TEXT("saved")));
    }

    return true;
}
