#pragma once

#include "Providers/ICortexGenProvider.h"

/**
 * Meshy (https://meshy.ai) provider.
 * Supports: MeshFromText, MeshFromImage, MeshFromBoth, Texturing.
 */
class FCortexGenMeshyProvider : public ICortexGenProvider
{
public:
    explicit FCortexGenMeshyProvider(const FString& InApiKey);

    FName GetProviderId() const override;
    FText GetDisplayName() const override;
    ECortexGenCapability GetCapabilities() const override;

    void SubmitJob(const FCortexGenJobRequest& Request, FOnGenJobSubmitted OnComplete) override;
    void PollJobStatus(const FString& ProviderJobId, FOnGenJobStatusReceived OnComplete) override;
    void CancelJob(const FString& ProviderJobId, FOnGenJobCancelled OnComplete) override;
    void DownloadResult(const FString& ResultUrl, const FString& LocalPath, FOnGenDownloadComplete OnComplete) override;

private:
    TSharedRef<class IHttpRequest> CreateRequest(const FString& Verb, const FString& Endpoint) const;
    static FString BaseUrl();

    FString ApiKey;
};
