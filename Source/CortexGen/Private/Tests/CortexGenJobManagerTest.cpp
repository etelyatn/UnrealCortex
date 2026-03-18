#include "Misc/AutomationTest.h"
#include "Operations/CortexGenJobManager.h"
#include "Providers/ICortexGenProvider.h"
#include "CortexGenSettings.h"

namespace
{

/** Mock provider that records calls and returns configurable results */
class FTestGenProvider : public ICortexGenProvider
{
public:
    FName GetProviderId() const override { return FName(TEXT("test")); }
    FText GetDisplayName() const override { return FText::FromString(TEXT("Test")); }
    ECortexGenCapability GetCapabilities() const override
    {
        return ECortexGenCapability::MeshFromText | ECortexGenCapability::MeshFromImage
            | ECortexGenCapability::Texturing;
    }

    void SubmitJob(const FCortexGenJobRequest& Request, FOnGenJobSubmitted OnComplete) override
    {
        SubmitCount++;
        FCortexGenSubmitResult Result;
        Result.bSuccess = bSubmitSucceeds;
        Result.ProviderJobId = bSubmitSucceeds ? TEXT("provider_job_1") : TEXT("");
        Result.ErrorMessage = bSubmitSucceeds ? TEXT("") : TEXT("Submit failed");
        OnComplete.ExecuteIfBound(Result);
    }

    void PollJobStatus(const FString& ProviderJobId, FOnGenJobStatusReceived OnComplete) override
    {
        PollCount++;
        OnComplete.ExecuteIfBound(NextPollResult);
    }

    void CancelJob(const FString& ProviderJobId, FOnGenJobCancelled OnComplete) override
    {
        CancelCount++;
        OnComplete.ExecuteIfBound(true);
    }

    void DownloadResult(const FString& ResultUrl, const FString& LocalPath,
        FOnGenDownloadComplete OnComplete) override
    {
        DownloadCount++;
        OnComplete.ExecuteIfBound(bDownloadSucceeds, bDownloadSucceeds ? TEXT("") : TEXT("Download error"));
    }

    bool bSubmitSucceeds = true;
    bool bDownloadSucceeds = true;
    FCortexGenPollResult NextPollResult;
    int32 SubmitCount = 0;
    int32 PollCount = 0;
    int32 CancelCount = 0;
    int32 DownloadCount = 0;
};

} // anonymous namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerSubmitTest,
    "Cortex.Gen.JobManager.SubmitJob",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerSubmitTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    FCortexGenJobManager Manager;
    Manager.RegisterProvider(Provider);

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("a wooden barrel");

    FString JobId;
    FString Error;
    bool bSuccess = Manager.SubmitJob(TEXT("test"), Request, JobId, Error);

    TestTrue(TEXT("Submit should succeed"), bSuccess);
    TestTrue(TEXT("JobId should start with gen_"), JobId.StartsWith(TEXT("gen_")));
    TestEqual(TEXT("Provider should have been called once"), Provider->SubmitCount, 1);

    // Verify job state
    const FCortexGenJobState* State = Manager.GetJobState(JobId);
    TestNotNull(TEXT("Job state should exist"), State);
    if (State)
    {
        TestEqual(TEXT("Status should be Pending or Processing"),
            State->Status == ECortexGenJobStatus::Pending ||
            State->Status == ECortexGenJobStatus::Processing, true);
        TestEqual(TEXT("Provider should be 'test'"), State->Provider, FString(TEXT("test")));
        TestEqual(TEXT("Prompt should match"), State->Prompt, FString(TEXT("a wooden barrel")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerSubmitFailTest,
    "Cortex.Gen.JobManager.SubmitJobFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerSubmitFailTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    Provider->bSubmitSucceeds = false;
    FCortexGenJobManager Manager;
    Manager.RegisterProvider(Provider);

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("test");

    FString JobId;
    FString Error;
    bool bSuccess = Manager.SubmitJob(TEXT("test"), Request, JobId, Error);

    TestFalse(TEXT("Submit should fail"), bSuccess);
    TestFalse(TEXT("Error should not be empty"), Error.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerValidationNoApiKeyTest,
    "Cortex.Gen.JobManager.ValidationNoProvider",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerValidationNoApiKeyTest::RunTest(const FString& Parameters)
{
    FCortexGenJobManager Manager;
    // No providers registered

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("test");

    FString JobId;
    FString Error;
    bool bSuccess = Manager.SubmitJob(TEXT("nonexistent"), Request, JobId, Error);

    TestFalse(TEXT("Submit should fail with no provider"), bSuccess);
    TestTrue(TEXT("Error should mention provider"), Error.Contains(TEXT("provider")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerCapabilityCheckTest,
    "Cortex.Gen.JobManager.CapabilityCheck",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerCapabilityCheckTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    FCortexGenJobManager Manager;
    Manager.RegisterProvider(Provider);

    // ImageFromText is not in test provider's capabilities
    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::ImageFromText;
    Request.Prompt = TEXT("test");

    FString JobId;
    FString Error;
    bool bSuccess = Manager.SubmitJob(TEXT("test"), Request, JobId, Error);

    TestFalse(TEXT("Submit should fail — capability not supported"), bSuccess);
    TestTrue(TEXT("Error should mention capability"), Error.Contains(TEXT("does not support")));
    TestEqual(TEXT("Provider should not have been called"), Provider->SubmitCount, 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerConcurrencyLimitTest,
    "Cortex.Gen.JobManager.ConcurrencyLimit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerConcurrencyLimitTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    FCortexGenJobManager Manager;
    Manager.RegisterProvider(Provider);
    Manager.SetMaxConcurrentJobs(1);

    // Submit first job — should succeed
    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("first");

    FString JobId1, Error1;
    bool bSuccess1 = Manager.SubmitJob(TEXT("test"), Request, JobId1, Error1);
    TestTrue(TEXT("First submit should succeed"), bSuccess1);

    // Submit second job — should fail (limit = 1)
    Request.Prompt = TEXT("second");
    FString JobId2, Error2;
    bool bSuccess2 = Manager.SubmitJob(TEXT("test"), Request, JobId2, Error2);
    TestFalse(TEXT("Second submit should fail — limit reached"), bSuccess2);
    TestTrue(TEXT("Error should mention concurrent"), Error2.Contains(TEXT("concurrent")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerCancelTest,
    "Cortex.Gen.JobManager.CancelJob",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerCancelTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    FCortexGenJobManager Manager;
    Manager.RegisterProvider(Provider);

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("test");

    FString JobId, Error;
    Manager.SubmitJob(TEXT("test"), Request, JobId, Error);

    bool bCancelled = Manager.CancelJob(JobId, Error);
    TestTrue(TEXT("Cancel should succeed"), bCancelled);

    const FCortexGenJobState* State = Manager.GetJobState(JobId);
    TestNotNull(TEXT("Job state should still exist"), State);
    if (State)
    {
        TestEqual(TEXT("Status should be Cancelled"),
            State->Status, ECortexGenJobStatus::Cancelled);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerListJobsTest,
    "Cortex.Gen.JobManager.ListJobs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerListJobsTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    FCortexGenJobManager Manager;
    Manager.RegisterProvider(Provider);

    // Submit two jobs
    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("job1");

    FString JobId1, JobId2, Error;
    Manager.SubmitJob(TEXT("test"), Request, JobId1, Error);
    Request.Prompt = TEXT("job2");
    Manager.SetMaxConcurrentJobs(5);
    Manager.SubmitJob(TEXT("test"), Request, JobId2, Error);

    TArray<FCortexGenJobState> Jobs = Manager.ListJobs();
    TestEqual(TEXT("Should have 2 jobs"), Jobs.Num(), 2);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerListProvidersTest,
    "Cortex.Gen.JobManager.ListProviders",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerListProvidersTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    FCortexGenJobManager Manager;
    Manager.RegisterProvider(Provider);

    TArray<TSharedPtr<ICortexGenProvider>> Providers = Manager.GetProviders();
    TestEqual(TEXT("Should have 1 provider"), Providers.Num(), 1);
    if (Providers.Num() > 0)
    {
        TestEqual(TEXT("Provider ID"), Providers[0]->GetProviderId(), FName(TEXT("test")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerDeleteJobTest,
    "Cortex.Gen.JobManager.DeleteJob",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerDeleteJobTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    FCortexGenJobManager Manager;
    Manager.RegisterProvider(Provider);

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("deleteme");

    FString JobId, Error;
    Manager.SubmitJob(TEXT("test"), Request, JobId, Error);

    // Cancel first (can't delete active jobs)
    Manager.CancelJob(JobId, Error);

    bool bDeleted = Manager.DeleteJob(JobId, Error);
    TestTrue(TEXT("Delete should succeed"), bDeleted);

    const FCortexGenJobState* State = Manager.GetJobState(JobId);
    TestNull(TEXT("Job state should be gone"), State);

    return true;
}
