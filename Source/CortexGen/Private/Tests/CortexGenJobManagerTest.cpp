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
        Result.ProviderJobId = bSubmitSucceeds ? TEXT("https://queue.fal.run/test/requests/abc/status") : TEXT("");
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

/** Helper to create a properly initialized JobManager for tests */
TSharedPtr<FCortexGenJobManager> CreateTestJobManager()
{
    auto Manager = MakeShared<FCortexGenJobManager>();
    Manager->Initialize();
    return Manager;
}

} // anonymous namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerSubmitTest,
    "Cortex.Gen.JobManager.SubmitJob",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerSubmitTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("a wooden barrel");

    FString JobId;
    FString Error;
    bool bSuccess = Manager->SubmitJob(TEXT("test"), Request, JobId, Error);

    TestTrue(TEXT("Submit should succeed"), bSuccess);
    TestTrue(TEXT("JobId should start with gen_"), JobId.StartsWith(TEXT("gen_")));
    TestEqual(TEXT("Provider should have been called once"), Provider->SubmitCount, 1);

    // Verify job state
    const FCortexGenJobState* State = Manager->GetJobState(JobId);
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
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("test");

    FString JobId;
    FString Error;
    bool bSuccess = Manager->SubmitJob(TEXT("test"), Request, JobId, Error);

    // SubmitJob returns true (job was queued) — provider failure propagates via job state
    TestTrue(TEXT("SubmitJob returns true (job queued)"), bSuccess);

    // Mock provider calls callback synchronously, so job is already in Failed state
    const FCortexGenJobState* State = Manager->GetJobState(JobId);
    TestNotNull(TEXT("Job state should exist"), State);
    if (State)
    {
        TestEqual(TEXT("Status should be Failed after provider failure"),
            State->Status, ECortexGenJobStatus::Failed);
        TestFalse(TEXT("ErrorMessage should not be empty"), State->ErrorMessage.IsEmpty());
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerValidationNoApiKeyTest,
    "Cortex.Gen.JobManager.ValidationNoProvider",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerValidationNoApiKeyTest::RunTest(const FString& Parameters)
{
    auto Manager = CreateTestJobManager();
    // No providers registered

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("test");

    FString JobId;
    FString Error;
    bool bSuccess = Manager->SubmitJob(TEXT("nonexistent"), Request, JobId, Error);

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
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);

    // ImageFromText is not in test provider's capabilities
    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::ImageFromText;
    Request.Prompt = TEXT("test");

    FString JobId;
    FString Error;
    bool bSuccess = Manager->SubmitJob(TEXT("test"), Request, JobId, Error);

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
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);
    Manager->SetMaxConcurrentJobs(1);

    // Submit first job — should succeed
    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("first");

    FString JobId1, Error1;
    bool bSuccess1 = Manager->SubmitJob(TEXT("test"), Request, JobId1, Error1);
    TestTrue(TEXT("First submit should succeed"), bSuccess1);

    // Submit second job — should fail (limit = 1)
    Request.Prompt = TEXT("second");
    FString JobId2, Error2;
    bool bSuccess2 = Manager->SubmitJob(TEXT("test"), Request, JobId2, Error2);
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
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("test");

    FString JobId, Error;
    Manager->SubmitJob(TEXT("test"), Request, JobId, Error);

    bool bCancelled = Manager->CancelJob(JobId, Error);
    TestTrue(TEXT("Cancel should succeed"), bCancelled);

    const FCortexGenJobState* State = Manager->GetJobState(JobId);
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
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);

    // Submit two jobs
    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("job1");

    FString JobId1, JobId2, Error;
    Manager->SubmitJob(TEXT("test"), Request, JobId1, Error);
    Request.Prompt = TEXT("job2");
    Manager->SetMaxConcurrentJobs(5);
    Manager->SubmitJob(TEXT("test"), Request, JobId2, Error);

    TArray<FCortexGenJobState> Jobs = Manager->ListJobs();
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
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);

    TArray<TSharedPtr<ICortexGenProvider>> Providers = Manager->GetProviders();
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
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("deleteme");

    FString JobId, Error;
    Manager->SubmitJob(TEXT("test"), Request, JobId, Error);

    // Cancel first (can't delete active jobs)
    Manager->CancelJob(JobId, Error);

    bool bDeleted = Manager->DeleteJob(JobId, Error);
    TestTrue(TEXT("Delete should succeed"), bDeleted);

    const FCortexGenJobState* State = Manager->GetJobState(JobId);
    TestNull(TEXT("Job state should be gone"), State);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerTimingTest,
    "Cortex.Gen.JobManager.Timing.GetAverageTime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerTimingTest::RunTest(const FString& Parameters)
{
    FCortexGenJobManager Manager;

    // No data yet — returns 0
    TestEqual(TEXT("No timing data returns 0"),
        Manager.GetAverageTime(TEXT("fal-ai/flux/dev")), 0.0f);

    // Record some timing
    Manager.RecordTiming(TEXT("fal-ai/flux/dev"), 2.3f);
    Manager.RecordTiming(TEXT("fal-ai/flux/dev"), 2.8f);
    Manager.RecordTiming(TEXT("fal-ai/flux/dev"), 2.1f);

    float Avg = Manager.GetAverageTime(TEXT("fal-ai/flux/dev"));
    TestTrue(TEXT("Average should be ~2.4"),
        FMath::IsNearlyEqual(Avg, 2.4f, 0.1f));

    // Different model is independent
    TestEqual(TEXT("Other model has no data"),
        Manager.GetAverageTime(TEXT("fal-ai/hyper3d/rodin")), 0.0f);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerTimingSlidingWindowTest,
    "Cortex.Gen.JobManager.Timing.SlidingWindow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerTimingSlidingWindowTest::RunTest(const FString& Parameters)
{
    FCortexGenJobManager Manager;

    // Fill beyond 20 samples
    for (int32 i = 0; i < 25; i++)
    {
        Manager.RecordTiming(TEXT("test-model"), 1.0f);
    }

    // Add a new value — should replace oldest
    Manager.RecordTiming(TEXT("test-model"), 100.0f);

    // Average should not be exactly 1.0 (the 100 value is included)
    float Avg = Manager.GetAverageTime(TEXT("test-model"));
    TestTrue(TEXT("Average should include recent sample"),
        Avg > 1.0f);

    return true;
}
