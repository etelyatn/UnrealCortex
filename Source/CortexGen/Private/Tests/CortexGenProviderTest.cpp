#include "Misc/AutomationTest.h"
#include "Providers/ICortexGenProvider.h"

namespace
{

/** Minimal mock provider for testing the interface contract */
class FMockGenProvider : public ICortexGenProvider
{
public:
    FName GetProviderId() const override { return FName(TEXT("mock")); }
    FText GetDisplayName() const override { return FText::FromString(TEXT("Mock Provider")); }
    ECortexGenCapability GetCapabilities() const override
    {
        return ECortexGenCapability::MeshFromText | ECortexGenCapability::Texturing;
    }

    void SubmitJob(const FCortexGenJobRequest& Request, FOnGenJobSubmitted OnComplete) override
    {
        FCortexGenSubmitResult Result;
        Result.bSuccess = true;
        Result.ProviderJobId = TEXT("mock_job_123");
        OnComplete.ExecuteIfBound(Result);
    }

    void PollJobStatus(const FString& ProviderJobId, FOnGenJobStatusReceived OnComplete) override
    {
        FCortexGenPollResult Result;
        Result.bSuccess = true;
        Result.Status = ECortexGenJobStatus::Processing;
        Result.Progress = 0.5f;
        OnComplete.ExecuteIfBound(Result);
    }

    void CancelJob(const FString& ProviderJobId, FOnGenJobCancelled OnComplete) override
    {
        OnComplete.ExecuteIfBound(true);
    }

    void DownloadResult(const FString& ResultUrl, const FString& LocalPath, FOnGenDownloadComplete OnComplete) override
    {
        OnComplete.ExecuteIfBound(true, FString());
    }
};

} // anonymous namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenProviderInterfaceTest,
    "Cortex.Gen.Provider.InterfaceContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenProviderInterfaceTest::RunTest(const FString& Parameters)
{
    TSharedPtr<ICortexGenProvider> Provider = MakeShared<FMockGenProvider>();

    TestEqual(TEXT("Provider ID"), Provider->GetProviderId(), FName(TEXT("mock")));
    TestFalse(TEXT("Display name should not be empty"),
        Provider->GetDisplayName().IsEmpty());

    // Capability checks
    ECortexGenCapability Caps = Provider->GetCapabilities();
    TestTrue(TEXT("Should support MeshFromText"),
        EnumHasAnyFlags(Caps, ECortexGenCapability::MeshFromText));
    TestTrue(TEXT("Should support Texturing"),
        EnumHasAnyFlags(Caps, ECortexGenCapability::Texturing));
    TestFalse(TEXT("Should not support ImageFromText"),
        EnumHasAnyFlags(Caps, ECortexGenCapability::ImageFromText));

    // Submit
    bool bSubmitCalled = false;
    FString SubmittedJobId;
    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("test prompt");

    Provider->SubmitJob(Request, FOnGenJobSubmitted::CreateLambda(
        [&](const FCortexGenSubmitResult& Result)
        {
            bSubmitCalled = true;
            SubmittedJobId = Result.ProviderJobId;
            TestTrue(TEXT("Submit should succeed"), Result.bSuccess);
        }));

    TestTrue(TEXT("Submit callback should have been called"), bSubmitCalled);
    TestEqual(TEXT("ProviderJobId"), SubmittedJobId, FString(TEXT("mock_job_123")));

    // Poll
    bool bPollCalled = false;
    Provider->PollJobStatus(TEXT("mock_job_123"), FOnGenJobStatusReceived::CreateLambda(
        [&](const FCortexGenPollResult& Result)
        {
            bPollCalled = true;
            TestTrue(TEXT("Poll should succeed"), Result.bSuccess);
            TestEqual(TEXT("Status should be Processing"), Result.Status, ECortexGenJobStatus::Processing);
            TestEqual(TEXT("Progress should be 0.5"), Result.Progress, 0.5f);
        }));

    TestTrue(TEXT("Poll callback should have been called"), bPollCalled);

    return true;
}
