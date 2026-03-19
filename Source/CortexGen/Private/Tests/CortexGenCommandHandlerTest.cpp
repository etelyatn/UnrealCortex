#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexGenCommandHandler.h"
#include "Operations/CortexGenJobManager.h"
#include "Dom/JsonObject.h"

namespace
{

/** Helper: create a router with an explicit job manager (avoids module global state) */
struct FGenTestContext
{
    TSharedPtr<FCortexGenJobManager> JobManager;
    FCortexCommandRouter Router;

    FGenTestContext()
    {
        JobManager = MakeShared<FCortexGenJobManager>();
        Router.RegisterDomain(TEXT("gen"), TEXT("Cortex Gen"), TEXT("1.0.0"),
            MakeShared<FCortexGenCommandHandler>(JobManager));
    }
};

} // anonymous namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCommandHandlerListProvidersTest,
    "Cortex.Gen.CommandHandler.ListProviders",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCommandHandlerListProvidersTest::RunTest(const FString& Parameters)
{
    FGenTestContext Ctx;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    FCortexCommandResult Result = Ctx.Router.Execute(TEXT("gen.list_providers"), Params);

    TestTrue(TEXT("list_providers should succeed"), Result.bSuccess);
    TestTrue(TEXT("Result should have data"), Result.Data.IsValid());

    if (Result.Data.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* ProvidersArray = nullptr;
        TestTrue(TEXT("Should have providers array"),
            Result.Data->TryGetArrayField(TEXT("providers"), ProvidersArray));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCommandHandlerGetConfigTest,
    "Cortex.Gen.CommandHandler.GetConfig",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCommandHandlerGetConfigTest::RunTest(const FString& Parameters)
{
    FGenTestContext Ctx;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    FCortexCommandResult Result = Ctx.Router.Execute(TEXT("gen.get_config"), Params);

    TestTrue(TEXT("get_config should succeed"), Result.bSuccess);
    if (Result.Data.IsValid())
    {
        FString DefaultProvider;
        TestTrue(TEXT("Should have default_provider"),
            Result.Data->TryGetStringField(TEXT("default_provider"), DefaultProvider));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCommandHandlerUnknownCommandTest,
    "Cortex.Gen.CommandHandler.UnknownCommand",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCommandHandlerUnknownCommandTest::RunTest(const FString& Parameters)
{
    FGenTestContext Ctx;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    FCortexCommandResult Result = Ctx.Router.Execute(TEXT("gen.nonexistent"), Params);

    TestFalse(TEXT("Unknown command should fail"), Result.bSuccess);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCommandHandlerStartMeshMissingPromptTest,
    "Cortex.Gen.CommandHandler.StartMeshMissingPrompt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCommandHandlerStartMeshMissingPromptTest::RunTest(const FString& Parameters)
{
    FGenTestContext Ctx;

    // Start mesh with no prompt — should fail (image-only mode is start_mesh with source_image_path but no prompt)
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    FCortexCommandResult Result = Ctx.Router.Execute(TEXT("gen.start_mesh"), Params);

    TestFalse(TEXT("start_mesh without prompt or image should fail"), Result.bSuccess);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCommandHandlerListJobsTest,
    "Cortex.Gen.CommandHandler.ListJobs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCommandHandlerListJobsTest::RunTest(const FString& Parameters)
{
    FGenTestContext Ctx;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    FCortexCommandResult Result = Ctx.Router.Execute(TEXT("gen.list_jobs"), Params);

    TestTrue(TEXT("list_jobs should succeed"), Result.bSuccess);
    if (Result.Data.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* JobsArray = nullptr;
        TestTrue(TEXT("Should have jobs array"),
            Result.Data->TryGetArrayField(TEXT("jobs"), JobsArray));

        double Count = 0;
        TestTrue(TEXT("Should have count"),
            Result.Data->TryGetNumberField(TEXT("count"), Count));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCommandHandlerSupportedCommandsTest,
    "Cortex.Gen.CommandHandler.SupportedCommands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCommandHandlerSupportedCommandsTest::RunTest(const FString& Parameters)
{
    // GetSupportedCommands is stateless — default constructor is safe here
    auto JobManager = MakeShared<FCortexGenJobManager>();
    FCortexGenCommandHandler Handler(JobManager);
    TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();

    // Verify all expected commands are present
    TSet<FString> CommandNames;
    for (const auto& Cmd : Commands)
    {
        CommandNames.Add(Cmd.Name);
    }

    TestTrue(TEXT("Should have start_mesh"), CommandNames.Contains(TEXT("start_mesh")));
    TestTrue(TEXT("Should have start_image"), CommandNames.Contains(TEXT("start_image")));
    TestTrue(TEXT("Should have start_texturing"), CommandNames.Contains(TEXT("start_texturing")));
    TestTrue(TEXT("Should have job_status"), CommandNames.Contains(TEXT("job_status")));
    TestTrue(TEXT("Should have list_jobs"), CommandNames.Contains(TEXT("list_jobs")));
    TestTrue(TEXT("Should have cancel_job"), CommandNames.Contains(TEXT("cancel_job")));
    TestTrue(TEXT("Should have retry_import"), CommandNames.Contains(TEXT("retry_import")));
    TestTrue(TEXT("Should have list_providers"), CommandNames.Contains(TEXT("list_providers")));
    TestTrue(TEXT("Should have delete_job"), CommandNames.Contains(TEXT("delete_job")));
    TestTrue(TEXT("Should have get_config"), CommandNames.Contains(TEXT("get_config")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCommandHandlerErrorCodeProviderNotFoundTest,
    "Cortex.Gen.CommandHandler.ErrorCode.ProviderNotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCommandHandlerErrorCodeProviderNotFoundTest::RunTest(const FString& Parameters)
{
    FGenTestContext Ctx;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("prompt"), TEXT("test"));
    Params->SetStringField(TEXT("provider"), TEXT("nonexistent"));
    FCortexCommandResult Result = Ctx.Router.Execute(TEXT("gen.start_mesh"), Params);

    TestFalse(TEXT("Should fail"), Result.bSuccess);
    TestEqual(TEXT("Error code should be PROVIDER_NOT_FOUND"),
        Result.ErrorCode, CortexErrorCodes::ProviderNotFound);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCommandHandlerErrorCodeJobNotFoundTest,
    "Cortex.Gen.CommandHandler.ErrorCode.JobNotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCommandHandlerErrorCodeJobNotFoundTest::RunTest(const FString& Parameters)
{
    FGenTestContext Ctx;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("job_id"), TEXT("nonexistent_job"));
    FCortexCommandResult Result = Ctx.Router.Execute(TEXT("gen.cancel_job"), Params);

    TestFalse(TEXT("Should fail"), Result.bSuccess);
    TestEqual(TEXT("Error code should be JOB_NOT_FOUND"),
        Result.ErrorCode, CortexErrorCodes::JobNotFound);

    return true;
}
