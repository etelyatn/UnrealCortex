#include "Misc/AutomationTest.h"
#include "Operations/CortexGenJobManager.h"
#include "Providers/ICortexGenProvider.h"
#include "CortexGenSettings.h"
#include "CortexGenCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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
    Manager->ClearTimingData();
    return Manager;
}

/** Distinct long prompt body (>512 chars) so omission vs. full retention is unambiguous. */
FString BuildLongGenJobPrompt(const TCHAR* Tag, TCHAR Fill)
{
    return FString(Tag) + FString::ChrN(600, Fill);
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
    ECortexGenError ErrorCode = ECortexGenError::None;
    bool bSuccess = Manager->SubmitJob(TEXT("test"), Request, JobId, Error, ErrorCode);

    TestTrue(TEXT("Submit should succeed"), bSuccess);
    TestTrue(TEXT("JobId should start with gen_"), JobId.StartsWith(TEXT("gen_")));
    TestEqual(TEXT("Error code should be None"), ErrorCode, ECortexGenError::None);
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
    ECortexGenError ErrorCode = ECortexGenError::None;
    bool bSuccess = Manager->SubmitJob(TEXT("test"), Request, JobId, Error, ErrorCode);

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
    ECortexGenError ErrorCode = ECortexGenError::None;
    bool bSuccess = Manager->SubmitJob(TEXT("nonexistent"), Request, JobId, Error, ErrorCode);

    TestFalse(TEXT("Submit should fail with no provider"), bSuccess);
    TestTrue(TEXT("Error should mention provider"), Error.Contains(TEXT("provider")));
    TestEqual(TEXT("Error code"), ErrorCode, ECortexGenError::ProviderNotFound);

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
    ECortexGenError ErrorCode = ECortexGenError::None;
    bool bSuccess = Manager->SubmitJob(TEXT("test"), Request, JobId, Error, ErrorCode);

    TestFalse(TEXT("Submit should fail — capability not supported"), bSuccess);
    TestTrue(TEXT("Error should mention capability"), Error.Contains(TEXT("does not support")));
    TestEqual(TEXT("Error code"), ErrorCode, ECortexGenError::CapabilityNotSupported);
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
    ECortexGenError ErrorCode1 = ECortexGenError::None;
    bool bSuccess1 = Manager->SubmitJob(TEXT("test"), Request, JobId1, Error1, ErrorCode1);
    TestTrue(TEXT("First submit should succeed"), bSuccess1);

    // Submit second job — should fail (limit = 1)
    Request.Prompt = TEXT("second");
    FString JobId2, Error2;
    ECortexGenError ErrorCode2 = ECortexGenError::None;
    bool bSuccess2 = Manager->SubmitJob(TEXT("test"), Request, JobId2, Error2, ErrorCode2);
    TestFalse(TEXT("Second submit should fail — limit reached"), bSuccess2);
    TestTrue(TEXT("Error should mention concurrent"), Error2.Contains(TEXT("concurrent")));
    TestEqual(TEXT("Error code"), ErrorCode2, ECortexGenError::JobLimitReached);

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
    ECortexGenError ErrorCode = ECortexGenError::None;
    Manager->SubmitJob(TEXT("test"), Request, JobId, Error, ErrorCode);

    bool bCancelled = Manager->CancelJob(JobId, Error, ErrorCode);
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
    ECortexGenError ErrorCode = ECortexGenError::None;
    Manager->SubmitJob(TEXT("test"), Request, JobId1, Error, ErrorCode);
    Request.Prompt = TEXT("job2");
    Manager->SetMaxConcurrentJobs(5);
    Manager->SubmitJob(TEXT("test"), Request, JobId2, Error, ErrorCode);

    TArray<FCortexGenJobState> Jobs = Manager->ListJobs();
    TestEqual(TEXT("Should have 2 jobs"), Jobs.Num(), 2);

    return true;
}

/**
 * list_jobs must default to include_prompt=false: rows keep identity/metadata (id, provider,
 * status, progress, created_at, completed_at) and the count, but omit the prompt body.
 * Explicit include_prompt=true restores the full prompt. Exercised through the real
 * FCortexGenCommandHandler over the isolated FTestGenProvider fixture (production insertion
 * path RegisterProvider/SubmitJob; no LoadJobs/SaveJobs, no provider network calls).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCommandHandlerListJobsPromptEmissionTest,
    "Cortex.Gen.CommandHandler.ListJobsPromptEmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCommandHandlerListJobsPromptEmissionTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);

    const FString Prompt1 = BuildLongGenJobPrompt(TEXT("alpha-prompt "), TEXT('a'));
    const FString Prompt2 = BuildLongGenJobPrompt(TEXT("beta-prompt "), TEXT('b'));

    FCortexGenJobRequest Request1;
    Request1.Type = ECortexGenJobType::MeshFromText;
    Request1.Prompt = Prompt1;

    FString JobId1, Error;
    ECortexGenError ErrorCode = ECortexGenError::None;
    TestTrue(TEXT("job1 submitted"), Manager->SubmitJob(TEXT("test"), Request1, JobId1, Error, ErrorCode));
    TestTrue(TEXT("job1 cancelled to terminal state"), Manager->CancelJob(JobId1, Error, ErrorCode));

    FCortexGenJobRequest Request2;
    Request2.Type = ECortexGenJobType::MeshFromText;
    Request2.Prompt = Prompt2;
    FString JobId2;
    TestTrue(TEXT("job2 submitted"), Manager->SubmitJob(TEXT("test"), Request2, JobId2, Error, ErrorCode));
    TestTrue(TEXT("job2 cancelled to terminal state"), Manager->CancelJob(JobId2, Error, ErrorCode));

    const FCortexGenJobState* State1 = Manager->GetJobState(JobId1);
    TestNotNull(TEXT("canonical state for job1 exists"), State1);

    FCortexGenCommandHandler Handler(Manager);

    // --- default list_jobs: prompt omitted, identity/metadata retained ---
    TSharedPtr<FJsonObject> DefaultParams = MakeShared<FJsonObject>();
    const FCortexCommandResult DefaultResult = Handler.Execute(TEXT("list_jobs"), DefaultParams);
    TestTrue(TEXT("default list_jobs succeeds"), DefaultResult.bSuccess);

    const TArray<TSharedPtr<FJsonValue>>* DefaultJobs = nullptr;
    TestTrue(TEXT("default list_jobs returns jobs array"),
        DefaultResult.Data.IsValid() && DefaultResult.Data->TryGetArrayField(TEXT("jobs"), DefaultJobs));
    if (DefaultJobs)
    {
        TestEqual(TEXT("default list_jobs returns every job"), DefaultJobs->Num(), 2);

        const TSharedPtr<FJsonValue>* DefaultRow = DefaultJobs->FindByPredicate(
            [&JobId1](const TSharedPtr<FJsonValue>& Item)
            {
                FString Id;
                const TSharedPtr<FJsonObject> Obj = Item.IsValid() ? Item->AsObject() : nullptr;
                return Obj.IsValid() && Obj->TryGetStringField(TEXT("job_id"), Id) && Id == JobId1;
            });
        TestNotNull(TEXT("default list_jobs retains submitted job"), DefaultRow);
        if (DefaultRow)
        {
            const TSharedPtr<FJsonObject> Obj = (*DefaultRow)->AsObject();
            TestTrue(TEXT("default row is an object"), Obj.IsValid());
            if (Obj.IsValid())
            {
                TestFalse(TEXT("default list_jobs omits prompt body"), Obj->HasField(TEXT("prompt")));

                FString RowProvider, RowStatus;
                double RowProgress = -1.0;
                TestTrue(TEXT("row provider retained"), Obj->TryGetStringField(TEXT("provider"), RowProvider));
                TestEqual(TEXT("row provider matches"), RowProvider, FString(TEXT("test")));
                TestTrue(TEXT("row status retained"), Obj->TryGetStringField(TEXT("status"), RowStatus));
                TestEqual(TEXT("row status matches canonical"), RowStatus, FString(TEXT("cancelled")));
                TestTrue(TEXT("row progress retained"), Obj->TryGetNumberField(TEXT("progress"), RowProgress));

                if (State1)
                {
                    TestEqual(TEXT("row progress matches canonical"), RowProgress,
                        static_cast<double>(State1->Progress));

                    FString RowCreatedAt, RowCompletedAt;
                    TestTrue(TEXT("row created_at retained"),
                        Obj->TryGetStringField(TEXT("created_at"), RowCreatedAt));
                    TestEqual(TEXT("row created_at matches canonical"), RowCreatedAt, State1->CreatedAt);
                    TestTrue(TEXT("row completed_at retained"),
                        Obj->TryGetStringField(TEXT("completed_at"), RowCompletedAt));
                    TestEqual(TEXT("row completed_at matches canonical"), RowCompletedAt, State1->CompletedAt);
                }
            }
        }
    }

    double DefaultCount = -1.0;
    TestTrue(TEXT("default count retained"),
        DefaultResult.Data.IsValid() && DefaultResult.Data->TryGetNumberField(TEXT("count"), DefaultCount));
    TestEqual(TEXT("default count equals returned rows"), DefaultCount, 2.0);

    // --- explicit include_prompt=false also omits the body on every row ---
    TSharedPtr<FJsonObject> OmitParams = MakeShared<FJsonObject>();
    OmitParams->SetBoolField(TEXT("include_prompt"), false);
    const FCortexCommandResult OmitResult = Handler.Execute(TEXT("list_jobs"), OmitParams);
    TestTrue(TEXT("list_jobs include_prompt=false succeeds"), OmitResult.bSuccess);
    TestTrue(TEXT("include_prompt=false returns response data"), OmitResult.Data.IsValid());
    const TArray<TSharedPtr<FJsonValue>>* OmitJobs = nullptr;
    TestTrue(TEXT("include_prompt=false returns jobs array"),
        OmitResult.Data.IsValid() && OmitResult.Data->TryGetArrayField(TEXT("jobs"), OmitJobs));
    double OmitCount = -1.0;
    TestTrue(TEXT("include_prompt=false count present"),
        OmitResult.Data.IsValid() && OmitResult.Data->TryGetNumberField(TEXT("count"), OmitCount));
    TestEqual(TEXT("include_prompt=false count equals returned rows"), OmitCount, 2.0);
    if (OmitJobs)
    {
        TestEqual(TEXT("include_prompt=false returns every job"), OmitJobs->Num(), 2);

        bool bAnyRowCarriedPrompt = false;
        for (const TSharedPtr<FJsonValue>& Item : *OmitJobs)
        {
            const TSharedPtr<FJsonObject> Obj = Item.IsValid() ? Item->AsObject() : nullptr;
            TestTrue(TEXT("include_prompt=false row is an object"), Obj.IsValid());
            if (Obj.IsValid())
            {
                TestTrue(TEXT("include_prompt=false row retains job_id"),
                    Obj->HasTypedField<EJson::String>(TEXT("job_id")));
                TestTrue(TEXT("include_prompt=false row retains provider"),
                    Obj->HasTypedField<EJson::String>(TEXT("provider")));
                TestTrue(TEXT("include_prompt=false row retains status"),
                    Obj->HasTypedField<EJson::String>(TEXT("status")));
                TestTrue(TEXT("include_prompt=false row retains progress"),
                    Obj->HasTypedField<EJson::Number>(TEXT("progress")));
                TestTrue(TEXT("include_prompt=false row retains created_at"),
                    Obj->HasTypedField<EJson::String>(TEXT("created_at")));
                TestTrue(TEXT("include_prompt=false row retains completed_at"),
                    Obj->HasTypedField<EJson::String>(TEXT("completed_at")));
                if (Obj->HasField(TEXT("prompt")))
                {
                    bAnyRowCarriedPrompt = true;
                }
            }
        }
        TestFalse(TEXT("explicit include_prompt=false omits every prompt body"), bAnyRowCarriedPrompt);
    }

    // --- include_prompt=true restores the full prompt body ---
    TSharedPtr<FJsonObject> PromptParams = MakeShared<FJsonObject>();
    PromptParams->SetBoolField(TEXT("include_prompt"), true);
    const FCortexCommandResult PromptResult = Handler.Execute(TEXT("list_jobs"), PromptParams);
    TestTrue(TEXT("list_jobs include_prompt=true succeeds"), PromptResult.bSuccess);
    TestTrue(TEXT("include_prompt=true returns response data"), PromptResult.Data.IsValid());
    const TArray<TSharedPtr<FJsonValue>>* PromptJobs = nullptr;
    TestTrue(TEXT("include_prompt=true returns jobs array"),
        PromptResult.Data.IsValid() && PromptResult.Data->TryGetArrayField(TEXT("jobs"), PromptJobs));
    double PromptCount = -1.0;
    TestTrue(TEXT("include_prompt=true count present"),
        PromptResult.Data.IsValid() && PromptResult.Data->TryGetNumberField(TEXT("count"), PromptCount));
    TestEqual(TEXT("include_prompt=true count equals returned rows"), PromptCount, 2.0);
    if (PromptJobs)
    {
        TestEqual(TEXT("include_prompt=true returns every job"), PromptJobs->Num(), 2);

        const TSharedPtr<FJsonValue>* PromptRow = PromptJobs->FindByPredicate(
            [&JobId1](const TSharedPtr<FJsonValue>& Item)
            {
                FString Id;
                const TSharedPtr<FJsonObject> Obj = Item.IsValid() ? Item->AsObject() : nullptr;
                return Obj.IsValid() && Obj->TryGetStringField(TEXT("job_id"), Id) && Id == JobId1;
            });
        TestNotNull(TEXT("submitted job retained"), PromptRow);
        if (PromptRow)
        {
            const TSharedPtr<FJsonObject> Obj = PromptRow->IsValid() ? (*PromptRow)->AsObject() : nullptr;
            TestTrue(TEXT("include_prompt=true row is an object"), Obj.IsValid());
            if (Obj.IsValid())
            {
                FString RowId, RowProvider, RowStatus, RowCreatedAt, RowCompletedAt;
                double RowProgress = -1.0;
                TestTrue(TEXT("include_prompt=true row retains job_id"),
                    Obj->TryGetStringField(TEXT("job_id"), RowId));
                TestEqual(TEXT("include_prompt=true row job_id matches"), RowId, JobId1);
                TestTrue(TEXT("include_prompt=true row retains provider"),
                    Obj->TryGetStringField(TEXT("provider"), RowProvider));
                TestEqual(TEXT("include_prompt=true row provider matches"), RowProvider, FString(TEXT("test")));
                TestTrue(TEXT("include_prompt=true row retains status"),
                    Obj->TryGetStringField(TEXT("status"), RowStatus));
                TestEqual(TEXT("include_prompt=true row status matches canonical"), RowStatus,
                    FString(TEXT("cancelled")));
                TestTrue(TEXT("include_prompt=true row retains progress"),
                    Obj->TryGetNumberField(TEXT("progress"), RowProgress));
                TestTrue(TEXT("include_prompt=true row retains created_at"),
                    Obj->TryGetStringField(TEXT("created_at"), RowCreatedAt));
                TestTrue(TEXT("include_prompt=true row retains completed_at"),
                    Obj->TryGetStringField(TEXT("completed_at"), RowCompletedAt));

                FString RowPrompt;
                TestTrue(TEXT("prompt present on request"),
                    Obj->TryGetStringField(TEXT("prompt"), RowPrompt));
                const FCortexGenJobState* Expected = State1;
                TestEqual(TEXT("prompt retained on request"), RowPrompt,
                    Expected ? Expected->Prompt : FString(TEXT("<missing canonical state>")));
                TestEqual(TEXT("include_prompt=true row progress matches canonical"), RowProgress,
                    Expected ? static_cast<double>(Expected->Progress) : -1.0);
                TestEqual(TEXT("include_prompt=true row created_at matches canonical"), RowCreatedAt,
                    Expected ? Expected->CreatedAt : FString(TEXT("<missing canonical state>")));
                TestEqual(TEXT("include_prompt=true row completed_at matches canonical"), RowCompletedAt,
                    Expected ? Expected->CompletedAt : FString(TEXT("<missing canonical state>")));
            }
        }
    }

    return true;
}

/**
 * native limit semantics must stay unchanged: limit=0 is native-unlimited (never reinterpreted
 * as a cached-pagination request), while a positive limit returns the bounded native page.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCommandHandlerListJobsNativeLimitTest,
    "Cortex.Gen.CommandHandler.ListJobsNativeLimitSemantics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCommandHandlerListJobsNativeLimitTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);
    Manager->SetMaxConcurrentJobs(10);

    const TCHAR Fills[] = { TEXT('0'), TEXT('1'), TEXT('2') };
    for (int32 Index = 0; Index < 3; ++Index)
    {
        FCortexGenJobRequest Request;
        Request.Type = ECortexGenJobType::MeshFromText;
        Request.Prompt = BuildLongGenJobPrompt(TEXT("limit-prompt "), Fills[Index]);

        FString JobId, Error;
        ECortexGenError ErrorCode = ECortexGenError::None;
        TestTrue(TEXT("job submitted"), Manager->SubmitJob(TEXT("test"), Request, JobId, Error, ErrorCode));
        TestTrue(TEXT("job cancelled to terminal state"), Manager->CancelJob(JobId, Error, ErrorCode));
    }

    FCortexGenCommandHandler Handler(Manager);

    TSharedPtr<FJsonObject> ZeroParams = MakeShared<FJsonObject>();
    ZeroParams->SetNumberField(TEXT("limit"), 0);
    const FCortexCommandResult ZeroResult = Handler.Execute(TEXT("list_jobs"), ZeroParams);
    double ZeroCount = -1.0;
    TestTrue(TEXT("limit=0 list_jobs succeeds"), ZeroResult.bSuccess);
    TestTrue(TEXT("limit=0 count present"),
        ZeroResult.Data.IsValid() && ZeroResult.Data->TryGetNumberField(TEXT("count"), ZeroCount));
    TestEqual(TEXT("native limit=0 stays unlimited (not a cached-pagination request)"), ZeroCount, 3.0);

    TSharedPtr<FJsonObject> OneParams = MakeShared<FJsonObject>();
    OneParams->SetNumberField(TEXT("limit"), 1);
    const FCortexCommandResult OneResult = Handler.Execute(TEXT("list_jobs"), OneParams);
    double OneCount = -1.0;
    TestTrue(TEXT("limit=1 list_jobs succeeds"), OneResult.bSuccess);
    TestTrue(TEXT("limit=1 count present"),
        OneResult.Data.IsValid() && OneResult.Data->TryGetNumberField(TEXT("count"), OneCount));
    TestEqual(TEXT("positive limit returns the existing bounded native page"), OneCount, 1.0);

    return true;
}

/**
 * job_status keeps returning the full prompt regardless of the list_jobs default.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCommandHandlerJobStatusPromptRetainedTest,
    "Cortex.Gen.CommandHandler.JobStatusPromptRetained",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCommandHandlerJobStatusPromptRetainedTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = BuildLongGenJobPrompt(TEXT("status-prompt "), TEXT('s'));

    FString JobId, Error;
    ECortexGenError ErrorCode = ECortexGenError::None;
    TestTrue(TEXT("job submitted"), Manager->SubmitJob(TEXT("test"), Request, JobId, Error, ErrorCode));
    TestTrue(TEXT("job cancelled to terminal state"), Manager->CancelJob(JobId, Error, ErrorCode));

    const FCortexGenJobState* State = Manager->GetJobState(JobId);
    TestNotNull(TEXT("canonical state exists"), State);

    FCortexGenCommandHandler Handler(Manager);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("job_id"), JobId);
    const FCortexCommandResult Result = Handler.Execute(TEXT("job_status"), Params);
    TestTrue(TEXT("job_status succeeds"), Result.bSuccess);
    TestTrue(TEXT("job_status returns response data"), Result.Data.IsValid());

    FString RowId, RowStatus, RowPrompt;
    TestTrue(TEXT("job_status retains job_id field"),
        Result.Data.IsValid() && Result.Data->TryGetStringField(TEXT("job_id"), RowId));
    TestEqual(TEXT("job_status job_id matches"), RowId, JobId);
    TestTrue(TEXT("job_status retains status field"),
        Result.Data.IsValid() && Result.Data->TryGetStringField(TEXT("status"), RowStatus));
    TestEqual(TEXT("job_status status matches canonical"), RowStatus, FString(TEXT("cancelled")));
    TestTrue(TEXT("job_status retains prompt field"),
        Result.Data.IsValid() && Result.Data->TryGetStringField(TEXT("prompt"), RowPrompt));
    TestEqual(TEXT("job_status retains the full prompt"), RowPrompt,
        State ? State->Prompt : FString(TEXT("<missing canonical state>")));

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
    ECortexGenError ErrorCode = ECortexGenError::None;
    Manager->SubmitJob(TEXT("test"), Request, JobId, Error, ErrorCode);

    // Cancel first (can't delete active jobs)
    Manager->CancelJob(JobId, Error, ErrorCode);

    bool bDeleted = Manager->DeleteJob(JobId, Error, ErrorCode);
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
    auto Manager = CreateTestJobManager();

    // No data yet — returns 0
    TestEqual(TEXT("No timing data returns 0"),
        Manager->GetAverageTime(TEXT("fal-ai/flux/dev")), 0.0f);

    // Record some timing
    Manager->RecordTiming(TEXT("fal-ai/flux/dev"), 2.3f);
    Manager->RecordTiming(TEXT("fal-ai/flux/dev"), 2.8f);
    Manager->RecordTiming(TEXT("fal-ai/flux/dev"), 2.1f);

    float Avg = Manager->GetAverageTime(TEXT("fal-ai/flux/dev"));
    TestTrue(TEXT("Average should be ~2.4"),
        FMath::IsNearlyEqual(Avg, 2.4f, 0.1f));

    // Different model is independent
    TestEqual(TEXT("Other model has no data"),
        Manager->GetAverageTime(TEXT("fal-ai/hyper3d/rodin")), 0.0f);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerTimingSlidingWindowTest,
    "Cortex.Gen.JobManager.Timing.SlidingWindow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerTimingSlidingWindowTest::RunTest(const FString& Parameters)
{
    auto Manager = CreateTestJobManager();

    // Fill beyond 20 samples
    for (int32 i = 0; i < 25; i++)
    {
        Manager->RecordTiming(TEXT("test-model"), 1.0f);
    }

    // Add a new value — should replace oldest
    Manager->RecordTiming(TEXT("test-model"), 100.0f);

    // Average should not be exactly 1.0 (the 100 value is included)
    float Avg = Manager->GetAverageTime(TEXT("test-model"));
    TestTrue(TEXT("Average should include recent sample"),
        Avg > 1.0f);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobManagerMaxJobHistoryTest,
    "Cortex.Gen.JobManager.MaxJobHistory",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobManagerMaxJobHistoryTest::RunTest(const FString& Parameters)
{
    auto Provider = MakeShared<FTestGenProvider>();
    auto Manager = CreateTestJobManager();
    Manager->RegisterProvider(Provider);
    Manager->SetMaxConcurrentJobs(10);

    // Submit 5 jobs and cancel them (making them terminal)
    TArray<FString> JobIds;
    for (int32 i = 0; i < 5; i++)
    {
        FCortexGenJobRequest Request;
        Request.Type = ECortexGenJobType::MeshFromText;
        Request.Prompt = FString::Printf(TEXT("job_%d"), i);

        FString JobId, Error;
        ECortexGenError ErrorCode;
        Manager->SubmitJob(TEXT("test"), Request, JobId, Error, ErrorCode);
        Manager->CancelJob(JobId, Error, ErrorCode);
        JobIds.Add(JobId);
    }

    TestEqual(TEXT("Should have 5 jobs before trim"), Manager->ListJobs().Num(), 5);

    // Trim to max 3 — should remove 2 terminal jobs
    Manager->TrimJobHistory(3);

    TArray<FCortexGenJobState> Remaining = Manager->ListJobs();
    TestEqual(TEXT("Should have 3 jobs after trim"), Remaining.Num(), 3);

    // Verify exactly 2 were removed (which 2 depends on sort stability with same timestamps)
    int32 RemovedCount = 0;
    for (const FString& Id : JobIds)
    {
        if (!Manager->GetJobState(Id))
        {
            RemovedCount++;
        }
    }
    TestEqual(TEXT("Exactly 2 jobs should have been removed"), RemovedCount, 2);

    return true;
}
