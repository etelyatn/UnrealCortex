#pragma once

#include "Providers/ICortexGenProvider.h"

/**
 * fal.ai Queue API provider.
 * Supports: MeshFromText, MeshFromImage.
 * Default model: fal-ai/hyper3d/rodin (GLB output, PBR materials).
 *
 * v1 limitation: SourceImagePath must be an http/https URL.
 * Local file upload is not supported.
 */
class FCortexGenFalProvider : public ICortexGenProvider
{
public:
    explicit FCortexGenFalProvider(const FString& InApiKey, const FString& InModelId);

    FName GetProviderId() const override;
    FText GetDisplayName() const override;
    ECortexGenCapability GetCapabilities() const override;

    void SubmitJob(const FCortexGenJobRequest& Request, FOnGenJobSubmitted OnComplete) override;
    void PollJobStatus(const FString& ProviderJobId, FOnGenJobStatusReceived OnComplete) override;
    void CancelJob(const FString& ProviderJobId, FOnGenJobCancelled OnComplete) override;
    void DownloadResult(const FString& ResultUrl, const FString& LocalPath, FOnGenDownloadComplete OnComplete) override;

protected:
    FString BuildSubmitBody(const FCortexGenJobRequest& Request) const;
    FCortexGenPollResult ParsePollResponse(const FString& JsonBody) const;

private:
    TSharedRef<class IHttpRequest> CreateRequest(const FString& Verb, const FString& Url) const;
    FString SubmitUrl() const;
    FString StatusUrl(const FString& RequestId) const;
    FString FetchResultUrl(const FString& RequestId) const;
    FString CancelUrl(const FString& RequestId) const;

    FString ApiKey;
    FString ModelId;
};
