#pragma once

#include "CoreMinimal.h"
#include "CortexGenTypes.h"

/** Result of a provider job submission */
struct FCortexGenSubmitResult
{
    bool bSuccess = false;
    FString ProviderJobId;
    FString ErrorMessage;
};

/** Result of a provider job status poll */
struct FCortexGenPollResult
{
    bool bSuccess = false;
    ECortexGenJobStatus Status = ECortexGenJobStatus::Pending;
    float Progress = 0.0f;          // 0.0-1.0
    FString ResultUrl;              // CDN URL when complete
    FString ErrorMessage;
};

DECLARE_DELEGATE_OneParam(FOnGenJobSubmitted, const FCortexGenSubmitResult&);
DECLARE_DELEGATE_OneParam(FOnGenJobStatusReceived, const FCortexGenPollResult&);
DECLARE_DELEGATE_OneParam(FOnGenJobCancelled, bool /* bSuccess */);
DECLARE_DELEGATE_TwoParams(FOnGenDownloadComplete, bool /* bSuccess */, const FString& /* ErrorMessage */);

/**
 * Provider interface for AI asset generation services.
 * Each provider (Meshy, Tripo3D, etc.) implements this interface.
 */
class ICortexGenProvider
{
public:
    virtual ~ICortexGenProvider() = default;

    virtual FName GetProviderId() const = 0;
    virtual FText GetDisplayName() const = 0;
    virtual ECortexGenCapability GetCapabilities() const = 0;

    virtual void SubmitJob(const FCortexGenJobRequest& Request, FOnGenJobSubmitted OnComplete) = 0;
    virtual void PollJobStatus(const FString& ProviderJobId, FOnGenJobStatusReceived OnComplete) = 0;
    virtual void CancelJob(const FString& ProviderJobId, FOnGenJobCancelled OnComplete) = 0;
    virtual void DownloadResult(const FString& ResultUrl, const FString& LocalPath, FOnGenDownloadComplete OnComplete) = 0;
};
