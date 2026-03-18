#pragma once

#include "Providers/ICortexGenProvider.h"

/**
 * fal.ai Queue API provider.
 * Supports: MeshFromText, MeshFromImage, ImageFromText.
 * Default mesh model: fal-ai/hyper3d/rodin (GLB output, PBR materials).
 * Default image model: fal-ai/flux/dev (fast, cheap prompt testing).
 *
 * v1 limitation: SourceImagePath must be an http/https URL.
 * Local file upload is not supported.
 */
class FCortexGenFalProvider : public ICortexGenProvider
{
public:
    explicit FCortexGenFalProvider(const FString& InApiKey, const FString& InModelId,
        const FString& InImageModelId = TEXT("fal-ai/flux/dev"),
        const FString& InQuality = TEXT("medium"));

    FName GetProviderId() const override;
    FText GetDisplayName() const override;
    ECortexGenCapability GetCapabilities() const override;

    void SubmitJob(const FCortexGenJobRequest& Request, FOnGenJobSubmitted OnComplete) override;
    void PollJobStatus(const FString& ProviderJobId, FOnGenJobStatusReceived OnComplete) override;
    void CancelJob(const FString& ProviderJobId, FOnGenJobCancelled OnComplete) override;
    void DownloadResult(const FString& ResultUrl, const FString& LocalPath, FOnGenDownloadComplete OnComplete) override;

protected:
    FString BuildSubmitBody(const FCortexGenJobRequest& Request) const;

    /** Build the submit URL for a request, using Request.ModelId if non-empty, otherwise falls back to type-based default. */
    FString SubmitUrlForRequest(const FCortexGenJobRequest& Request) const;

    /** Parse a fal.ai status poll JSON response into a poll result. Pure JSON logic, no state. */
    static FCortexGenPollResult ParsePollResponse(const FString& JsonBody);

    /** Extract download URL from a fal.ai result response, trying multiple known paths. */
    static FString ExtractResultUrl(const TSharedPtr<FJsonObject>& Json);

    /** Strip "/status" suffix from a fal.ai URL to derive the base request URL. */
    static FString StripStatusSuffix(const FString& Url);

private:
    TSharedRef<class IHttpRequest> CreateRequest(const FString& Verb, const FString& Url) const;
    FString SubmitUrlForType(ECortexGenJobType Type) const;

    /** Resolve model ID for the given job type (mesh vs image). */
    const FString& ModelIdForType(ECortexGenJobType Type) const;

    FString ApiKey;
    FString ModelId;
    FString ImageModelId;
    FString DefaultQuality;
};
