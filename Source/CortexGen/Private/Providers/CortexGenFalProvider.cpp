#include "Providers/CortexGenFalProvider.h"
#include "CortexGenModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

static const TCHAR* KFalQueueBase = TEXT("https://queue.fal.run");

/** Default timeout for poll/result HTTP requests (seconds). */
static constexpr float KFalHttpTimeoutSeconds = 30.0f;

FCortexGenFalProvider::FCortexGenFalProvider(const FString& InApiKey, const FString& InModelId,
    const FString& InImageModelId, const FString& InQuality)
    : ApiKey(InApiKey)
    , ModelId(InModelId)
    , ImageModelId(InImageModelId)
    , DefaultQuality(InQuality)
{
}

FName FCortexGenFalProvider::GetProviderId() const
{
    return FName(TEXT("fal"));
}

FText FCortexGenFalProvider::GetDisplayName() const
{
    return FText::FromString(TEXT("fal.ai"));
}

ECortexGenCapability FCortexGenFalProvider::GetCapabilities() const
{
    return ECortexGenCapability::MeshFromText
        | ECortexGenCapability::MeshFromImage
        | ECortexGenCapability::ImageFromText;
}

const FString& FCortexGenFalProvider::ModelIdForType(ECortexGenJobType Type) const
{
    return (Type == ECortexGenJobType::ImageFromText) ? ImageModelId : ModelId;
}

FString FCortexGenFalProvider::SubmitUrlForType(ECortexGenJobType Type) const
{
    return FString::Printf(TEXT("%s/%s"), KFalQueueBase, *ModelIdForType(Type));
}

FString FCortexGenFalProvider::SubmitUrlForRequest(const FCortexGenJobRequest& Request) const
{
    const FString& EffectiveModel = Request.ModelId.IsEmpty()
        ? ModelIdForType(Request.Type)
        : Request.ModelId;
    return FString::Printf(TEXT("%s/%s"), KFalQueueBase, *EffectiveModel);
}

FString FCortexGenFalProvider::StripStatusSuffix(const FString& Url)
{
    return Url.EndsWith(TEXT("/status"))
        ? Url.LeftChop(7)
        : Url;
}

TSharedRef<IHttpRequest> FCortexGenFalProvider::CreateRequest(
    const FString& Verb, const FString& Url) const
{
    TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
    Request->SetVerb(Verb);
    Request->SetURL(Url);
    Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Key %s"), *ApiKey));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetTimeout(KFalHttpTimeoutSeconds);
    return Request;
}

FString FCortexGenFalProvider::BuildSubmitBody(const FCortexGenJobRequest& Request) const
{
    // fal.ai Queue API expects fields at the top level (NOT wrapped in "input").
    TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();

    if (Request.Type == ECortexGenJobType::ImageFromText)
    {
        // Image generation (e.g., fal-ai/flux/dev).
        // Default image_size can be overridden via Request.Params.
        Body->SetStringField(TEXT("prompt"), Request.Prompt);
        Body->SetStringField(TEXT("image_size"), TEXT("landscape_4_3"));
    }
    else
    {
        // Mesh generation (e.g., fal-ai/hyper3d/rodin)
        if (!Request.Prompt.IsEmpty())
        {
            Body->SetStringField(TEXT("prompt"), Request.Prompt);
        }

        Body->SetStringField(TEXT("geometry_file_format"), TEXT("glb"));
        Body->SetStringField(TEXT("material"), TEXT("PBR"));

        // Quality: use param override or provider default
        FString Quality = DefaultQuality;
        if (const FString* QualityParam = Request.Params.Find(TEXT("quality")))
        {
            Quality = *QualityParam;
        }
        Body->SetStringField(TEXT("quality"), Quality);

        if (Request.Type == ECortexGenJobType::MeshFromImage ||
            Request.Type == ECortexGenJobType::MeshFromBoth)
        {
            if (!Request.SourceImagePath.IsEmpty())
            {
                TArray<TSharedPtr<FJsonValue>> UrlArray;
                UrlArray.Add(MakeShared<FJsonValueString>(Request.SourceImagePath));
                Body->SetArrayField(TEXT("input_image_urls"), UrlArray);
            }
        }
    }

    // Custom params override — can override any default (image_size, geometry_file_format, etc.)
    // Skips "quality" which is already handled above for mesh requests.
    for (const auto& Pair : Request.Params)
    {
        if (Pair.Key == TEXT("quality"))
        {
            continue;
        }
        Body->SetStringField(Pair.Key, Pair.Value);
    }

    FString BodyString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyString);
    FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
    return BodyString;
}

void FCortexGenFalProvider::SubmitJob(
    const FCortexGenJobRequest& Request, FOnGenJobSubmitted OnComplete)
{
    // Validate API key early
    if (ApiKey.IsEmpty())
    {
        UE_LOG(LogCortexGen, Warning, TEXT("fal.ai submit: API key not configured"));
        FCortexGenSubmitResult Result;
        Result.bSuccess = false;
        Result.ErrorMessage = TEXT("fal.ai API key not configured. Set it in Project Settings > Cortex Gen.");
        OnComplete.ExecuteIfBound(Result);
        return;
    }

    if (Request.Type == ECortexGenJobType::Texturing)
    {
        FCortexGenSubmitResult Result;
        Result.bSuccess = false;
        Result.ErrorMessage = TEXT("Unsupported job type for fal.ai provider");
        OnComplete.ExecuteIfBound(Result);
        return;
    }

    if ((Request.Type == ECortexGenJobType::MeshFromImage ||
         Request.Type == ECortexGenJobType::MeshFromBoth) &&
        !Request.SourceImagePath.IsEmpty() &&
        !Request.SourceImagePath.StartsWith(TEXT("http")))
    {
        FCortexGenSubmitResult Result;
        Result.bSuccess = false;
        Result.ErrorMessage = TEXT("fal.ai provider v1: local image files are not supported. "
            "Supply an https:// URL for SourceImagePath.");
        OnComplete.ExecuteIfBound(Result);
        return;
    }

    FString Url = SubmitUrlForRequest(Request);
    FString Body = BuildSubmitBody(Request);

    UE_LOG(LogCortexGen, Log, TEXT("fal.ai submit: POST %s"), *Url);
    UE_LOG(LogCortexGen, Verbose, TEXT("fal.ai submit body: %s"), *Body);

    TSharedRef<IHttpRequest> HttpRequest = CreateRequest(TEXT("POST"), Url);
    HttpRequest->SetContentAsString(Body);

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
        {
            FCortexGenSubmitResult Result;

            if (!bConnectedSuccessfully || !Response.IsValid())
            {
                UE_LOG(LogCortexGen, Warning, TEXT("fal.ai submit: HTTP connection failed"));
                Result.bSuccess = false;
                Result.ErrorMessage = TEXT("HTTP connection failed");
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            const int32 Code = Response->GetResponseCode();
            UE_LOG(LogCortexGen, Log, TEXT("fal.ai submit: HTTP %d"), Code);

            if (Code < 200 || Code >= 300)
            {
                UE_LOG(LogCortexGen, Warning, TEXT("fal.ai submit error: %s"),
                    *Response->GetContentAsString());
                Result.bSuccess = false;
                Result.ErrorMessage = FString::Printf(
                    TEXT("fal.ai API error %d: %s"),
                    Code, *Response->GetContentAsString());
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            TSharedPtr<FJsonObject> Json;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
                Response->GetContentAsString());
            if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
            {
                UE_LOG(LogCortexGen, Warning, TEXT("fal.ai submit: failed to parse response JSON"));
                Result.bSuccess = false;
                Result.ErrorMessage = TEXT("Failed to parse fal.ai submit response");
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            Result.bSuccess = true;
            // Always use the full status_url — avoids coupling poll/cancel to a specific model ID.
            FString StatusUrlField;
            if (Json->TryGetStringField(TEXT("status_url"), StatusUrlField) &&
                !StatusUrlField.IsEmpty())
            {
                Result.ProviderJobId = StatusUrlField;
            }
            else
            {
                // Construct full URL from response_url as fallback
                FString ResponseUrl;
                if (Json->TryGetStringField(TEXT("response_url"), ResponseUrl) &&
                    !ResponseUrl.IsEmpty())
                {
                    Result.ProviderJobId = ResponseUrl + TEXT("/status");
                }
                else
                {
                    Result.bSuccess = false;
                    Result.ErrorMessage = TEXT("fal.ai submit response missing status_url and response_url");
                    OnComplete.ExecuteIfBound(Result);
                    return;
                }
            }
            UE_LOG(LogCortexGen, Log, TEXT("fal.ai submit OK — provider_job_id=%s"),
                *Result.ProviderJobId);
            UE_LOG(LogCortexGen, Verbose, TEXT("fal.ai submit response: %s"),
                *Response->GetContentAsString());
            OnComplete.ExecuteIfBound(Result);
        });

    HttpRequest->ProcessRequest();
}

FCortexGenPollResult FCortexGenFalProvider::ParsePollResponse(const FString& JsonBody)
{
    FCortexGenPollResult Result;

    TSharedPtr<FJsonObject> Json;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonBody);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
    {
        Result.bSuccess = false;
        Result.ErrorMessage = TEXT("Failed to parse poll response JSON");
        return Result;
    }

    FString StatusStr;
    if (!Json->TryGetStringField(TEXT("status"), StatusStr) || StatusStr.IsEmpty())
    {
        Result.bSuccess = false;
        Result.ErrorMessage = TEXT("Poll response missing 'status' field");
        return Result;
    }

    Result.bSuccess = true;

    if (StatusStr == TEXT("COMPLETED"))
    {
        FString ErrorStr;
        if (Json->TryGetStringField(TEXT("error"), ErrorStr) && !ErrorStr.IsEmpty())
        {
            Result.Status = ECortexGenJobStatus::Failed;
            Result.ErrorMessage = ErrorStr;
        }
        else
        {
            Result.Status = ECortexGenJobStatus::Complete;
            Result.Progress = 1.0f;
        }
    }
    else if (StatusStr == TEXT("FAILED"))
    {
        Result.Status = ECortexGenJobStatus::Failed;
        FString ErrorStr;
        if (Json->TryGetStringField(TEXT("error"), ErrorStr))
        {
            Result.ErrorMessage = ErrorStr;
        }
        else
        {
            Result.ErrorMessage = TEXT("Generation failed at provider");
        }
    }
    else // IN_QUEUE, IN_PROGRESS, unknown
    {
        Result.Status = ECortexGenJobStatus::Processing;
        Result.Progress = 0.0f;
    }

    return Result;
}

FString FCortexGenFalProvider::ExtractResultUrl(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return FString();
    }

    // Try multiple known response structures from different fal.ai models:

    // 1. output.model_mesh.url (Rodin / Hyper3D)
    {
        const TSharedPtr<FJsonObject>* OutputObj = nullptr;
        if (Json->TryGetObjectField(TEXT("output"), OutputObj) && OutputObj->IsValid())
        {
            const TSharedPtr<FJsonObject>* MeshObj = nullptr;
            if ((*OutputObj)->TryGetObjectField(TEXT("model_mesh"), MeshObj) && MeshObj->IsValid())
            {
                FString Url;
                if ((*MeshObj)->TryGetStringField(TEXT("url"), Url) && !Url.IsEmpty())
                {
                    return Url;
                }
            }
        }
    }

    // 2. model_mesh.url (Rodin — flat variant)
    {
        const TSharedPtr<FJsonObject>* MeshObj = nullptr;
        if (Json->TryGetObjectField(TEXT("model_mesh"), MeshObj) && MeshObj->IsValid())
        {
            FString Url;
            if ((*MeshObj)->TryGetStringField(TEXT("url"), Url) && !Url.IsEmpty())
            {
                return Url;
            }
        }
    }

    // 3. model_glb.url (Hunyuan 3D)
    {
        const TSharedPtr<FJsonObject>* GlbObj = nullptr;
        if (Json->TryGetObjectField(TEXT("model_glb"), GlbObj) && GlbObj->IsValid())
        {
            FString Url;
            if ((*GlbObj)->TryGetStringField(TEXT("url"), Url) && !Url.IsEmpty())
            {
                return Url;
            }
        }
    }

    // 4. images[0].url (Flux / image models)
    {
        const TArray<TSharedPtr<FJsonValue>>* ImagesArray = nullptr;
        if (Json->TryGetArrayField(TEXT("images"), ImagesArray) && ImagesArray && ImagesArray->Num() > 0)
        {
            const TSharedPtr<FJsonObject>* ImgObj = nullptr;
            if ((*ImagesArray)[0]->TryGetObject(ImgObj) && ImgObj->IsValid())
            {
                FString Url;
                if ((*ImgObj)->TryGetStringField(TEXT("url"), Url) && !Url.IsEmpty())
                {
                    return Url;
                }
            }
        }
    }

    // 5. Fallback: top-level "url" field
    {
        FString Url;
        if (Json->TryGetStringField(TEXT("url"), Url) && !Url.IsEmpty())
        {
            return Url;
        }
    }

    return FString();
}

void FCortexGenFalProvider::PollJobStatus(
    const FString& ProviderJobId, FOnGenJobStatusReceived OnComplete)
{
    // ProviderJobId is always a full status_url (e.g., https://queue.fal.run/.../status).
    FString PollStatusUrl = ProviderJobId;
    FString PollResultUrl = StripStatusSuffix(ProviderJobId);
    FString AuthHeader = FString::Printf(TEXT("Key %s"), *ApiKey);

    UE_LOG(LogCortexGen, Verbose, TEXT("fal.ai poll: GET %s"), *PollStatusUrl);
    TSharedRef<IHttpRequest> HttpRequest = CreateRequest(TEXT("GET"), PollStatusUrl);

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [PollResultUrl, AuthHeader, OnComplete](
            FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
        {
            FCortexGenPollResult Result;

            if (!bConnectedSuccessfully || !Response.IsValid())
            {
                UE_LOG(LogCortexGen, Warning, TEXT("fal.ai poll: HTTP connection failed"));
                Result.bSuccess = false;
                Result.ErrorMessage = TEXT("HTTP connection failed");
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            const int32 Code = Response->GetResponseCode();

            // Handle rate-limiting: treat 429 as "still processing, retry next tick"
            if (Code == 429)
            {
                UE_LOG(LogCortexGen, Warning,
                    TEXT("fal.ai poll: rate-limited (HTTP 429), will retry next tick"));
                Result.bSuccess = true;
                Result.Status = ECortexGenJobStatus::Processing;
                Result.Progress = 0.0f;
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            if (Code < 200 || Code >= 300)
            {
                UE_LOG(LogCortexGen, Warning, TEXT("fal.ai poll HTTP %d: %s"),
                    Code, *Response->GetContentAsString());
                Result.bSuccess = false;
                Result.ErrorMessage = FString::Printf(
                    TEXT("fal.ai poll error %d: %s"),
                    Code, *Response->GetContentAsString());
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            UE_LOG(LogCortexGen, Verbose, TEXT("fal.ai poll response: %s"),
                *Response->GetContentAsString());

            // Reuse static ParsePollResponse — pure JSON logic, no instance state
            Result = ParsePollResponse(Response->GetContentAsString());

            if (Result.bSuccess && Result.Status == ECortexGenJobStatus::Complete)
            {
                UE_LOG(LogCortexGen, Log, TEXT("fal.ai: generation complete, fetching result from %s"),
                    *PollResultUrl);

                TSharedRef<IHttpRequest> ResultRequest = FHttpModule::Get().CreateRequest();
                ResultRequest->SetVerb(TEXT("GET"));
                ResultRequest->SetURL(PollResultUrl);
                ResultRequest->SetHeader(TEXT("Authorization"), AuthHeader);
                ResultRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
                ResultRequest->SetTimeout(KFalHttpTimeoutSeconds);

                ResultRequest->OnProcessRequestComplete().BindLambda(
                    [Result, OnComplete](
                        FHttpRequestPtr, FHttpResponsePtr ResResponse,
                        bool bResConnected) mutable
                    {
                        if (!bResConnected || !ResResponse.IsValid() ||
                            ResResponse->GetResponseCode() < 200 ||
                            ResResponse->GetResponseCode() >= 300)
                        {
                            UE_LOG(LogCortexGen, Warning,
                                TEXT("fal.ai result fetch failed: connected=%d code=%d"),
                                bResConnected,
                                ResResponse.IsValid() ? ResResponse->GetResponseCode() : 0);
                            Result.bSuccess = false;
                            Result.ErrorMessage = TEXT("Failed to fetch result from fal.ai");
                            OnComplete.ExecuteIfBound(Result);
                            return;
                        }

                        UE_LOG(LogCortexGen, Verbose, TEXT("fal.ai result response: %s"),
                            *ResResponse->GetContentAsString());

                        TSharedPtr<FJsonObject> ResJson;
                        TSharedRef<TJsonReader<>> ResReader =
                            TJsonReaderFactory<>::Create(ResResponse->GetContentAsString());
                        if (FJsonSerializer::Deserialize(ResReader, ResJson) &&
                            ResJson.IsValid())
                        {
                            Result.ResultUrl = ExtractResultUrl(ResJson);
                        }
                        if (Result.ResultUrl.IsEmpty())
                        {
                            UE_LOG(LogCortexGen, Warning,
                                TEXT("fal.ai: could not extract download URL from result response"));
                            Result.bSuccess = false;
                            Result.ErrorMessage = TEXT("Could not extract download URL from fal.ai result");
                        }
                        else
                        {
                            UE_LOG(LogCortexGen, Log, TEXT("fal.ai: download URL extracted: %s"),
                                *Result.ResultUrl);
                        }
                        OnComplete.ExecuteIfBound(Result);
                    });

                ResultRequest->ProcessRequest();
                return;
            }

            if (Result.bSuccess && Result.Status == ECortexGenJobStatus::Failed)
            {
                UE_LOG(LogCortexGen, Warning, TEXT("fal.ai: provider reported failure: %s"),
                    *Result.ErrorMessage);
            }

            OnComplete.ExecuteIfBound(Result);
        });

    HttpRequest->ProcessRequest();
}

void FCortexGenFalProvider::CancelJob(
    const FString& ProviderJobId, FOnGenJobCancelled OnComplete)
{
    FString CancelEndpoint = StripStatusSuffix(ProviderJobId) + TEXT("/cancel");
    UE_LOG(LogCortexGen, Log, TEXT("fal.ai cancel: PUT %s"), *CancelEndpoint);

    TSharedRef<IHttpRequest> HttpRequest = CreateRequest(TEXT("PUT"), CancelEndpoint);

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
        {
            bool bSuccess = bConnectedSuccessfully && Response.IsValid() &&
                Response->GetResponseCode() >= 200 && Response->GetResponseCode() < 300;
            UE_LOG(LogCortexGen, Log, TEXT("fal.ai cancel result: %s"),
                bSuccess ? TEXT("OK") : TEXT("Failed"));
            OnComplete.ExecuteIfBound(bSuccess);
        });

    HttpRequest->ProcessRequest();
}

void FCortexGenFalProvider::DownloadResult(
    const FString& ResultUrl, const FString& LocalPath, FOnGenDownloadComplete OnComplete)
{
    UE_LOG(LogCortexGen, Log, TEXT("fal.ai download: GET %s -> %s"), *ResultUrl, *LocalPath);

    TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
    HttpRequest->SetVerb(TEXT("GET"));
    HttpRequest->SetURL(ResultUrl);
    HttpRequest->SetTimeout(120.0f); // Downloads may be large — generous timeout

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [LocalPath, OnComplete](
            FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
        {
            if (!bConnectedSuccessfully || !Response.IsValid() ||
                Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
            {
                const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;
                UE_LOG(LogCortexGen, Warning,
                    TEXT("fal.ai download failed: connected=%d code=%d"),
                    bConnectedSuccessfully, Code);
                OnComplete.ExecuteIfBound(false,
                    FString::Printf(TEXT("Download failed (HTTP %d)"), Code));
                return;
            }

            const int64 ContentSize = Response->GetContent().Num();
            UE_LOG(LogCortexGen, Log, TEXT("fal.ai download complete: %lld bytes"), ContentSize);

            if (!FFileHelper::SaveArrayToFile(Response->GetContent(), *LocalPath))
            {
                UE_LOG(LogCortexGen, Warning, TEXT("fal.ai: failed to save file to %s"), *LocalPath);
                OnComplete.ExecuteIfBound(false, FString::Printf(
                    TEXT("Failed to save file to %s"), *LocalPath));
                return;
            }

            UE_LOG(LogCortexGen, Log, TEXT("fal.ai: saved to %s"), *LocalPath);
            OnComplete.ExecuteIfBound(true, FString());
        });

    HttpRequest->ProcessRequest();
}
