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

FCortexGenFalProvider::FCortexGenFalProvider(const FString& InApiKey, const FString& InModelId)
    : ApiKey(InApiKey)
    , ModelId(InModelId)
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
    return ECortexGenCapability::MeshFromText | ECortexGenCapability::MeshFromImage;
}

FString FCortexGenFalProvider::SubmitUrl() const
{
    return FString::Printf(TEXT("%s/%s"), KFalQueueBase, *ModelId);
}

FString FCortexGenFalProvider::StatusUrl(const FString& RequestId) const
{
    return FString::Printf(TEXT("%s/%s/requests/%s/status"), KFalQueueBase, *ModelId, *RequestId);
}

FString FCortexGenFalProvider::FetchResultUrl(const FString& RequestId) const
{
    return FString::Printf(TEXT("%s/%s/requests/%s"), KFalQueueBase, *ModelId, *RequestId);
}

FString FCortexGenFalProvider::CancelUrl(const FString& RequestId) const
{
    return FString::Printf(TEXT("%s/%s/requests/%s/cancel"), KFalQueueBase, *ModelId, *RequestId);
}

TSharedRef<IHttpRequest> FCortexGenFalProvider::CreateRequest(
    const FString& Verb, const FString& Url) const
{
    TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
    Request->SetVerb(Verb);
    Request->SetURL(Url);
    Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Key %s"), *ApiKey));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    return Request;
}

FString FCortexGenFalProvider::BuildSubmitBody(const FCortexGenJobRequest& Request) const
{
    TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
    InputObj->SetStringField(TEXT("geometry_file_format"), TEXT("glb"));
    InputObj->SetStringField(TEXT("material"), TEXT("PBR"));
    InputObj->SetStringField(TEXT("quality"), TEXT("medium"));

    if (!Request.Prompt.IsEmpty())
    {
        InputObj->SetStringField(TEXT("prompt"), Request.Prompt);
    }

    if (Request.Type == ECortexGenJobType::MeshFromImage ||
        Request.Type == ECortexGenJobType::MeshFromBoth)
    {
        if (!Request.SourceImagePath.IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> UrlArray;
            UrlArray.Add(MakeShared<FJsonValueString>(Request.SourceImagePath));
            InputObj->SetArrayField(TEXT("input_image_urls"), UrlArray);
        }
    }

    for (const auto& Pair : Request.Params)
    {
        InputObj->SetStringField(Pair.Key, Pair.Value);
    }

    TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetObjectField(TEXT("input"), InputObj);

    FString BodyString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyString);
    FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
    return BodyString;
}

void FCortexGenFalProvider::SubmitJob(
    const FCortexGenJobRequest& Request, FOnGenJobSubmitted OnComplete)
{
    if (Request.Type == ECortexGenJobType::Texturing ||
        Request.Type == ECortexGenJobType::ImageFromText)
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

    TSharedRef<IHttpRequest> HttpRequest = CreateRequest(TEXT("POST"), SubmitUrl());
    HttpRequest->SetContentAsString(BuildSubmitBody(Request));

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
        {
            FCortexGenSubmitResult Result;

            if (!bConnectedSuccessfully || !Response.IsValid())
            {
                Result.bSuccess = false;
                Result.ErrorMessage = TEXT("HTTP connection failed");
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            if (Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
            {
                Result.bSuccess = false;
                Result.ErrorMessage = FString::Printf(
                    TEXT("fal.ai API error %d: %s"),
                    Response->GetResponseCode(),
                    *Response->GetContentAsString());
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            TSharedPtr<FJsonObject> Json;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
                Response->GetContentAsString());
            if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
            {
                Result.bSuccess = false;
                Result.ErrorMessage = TEXT("Failed to parse fal.ai submit response");
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            Result.bSuccess = true;
            Result.ProviderJobId = Json->GetStringField(TEXT("request_id"));
            OnComplete.ExecuteIfBound(Result);
        });

    HttpRequest->ProcessRequest();
}

FCortexGenPollResult FCortexGenFalProvider::ParsePollResponse(const FString& JsonBody) const
{
    FCortexGenPollResult Result;

    TSharedPtr<FJsonObject> Json;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonBody);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
    {
        Result.bSuccess = false;
        Result.ErrorMessage = TEXT("Failed to parse poll response");
        return Result;
    }

    Result.bSuccess = true;
    FString StatusStr = Json->GetStringField(TEXT("status"));

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
    else // IN_QUEUE, IN_PROGRESS, unknown
    {
        Result.Status = ECortexGenJobStatus::Processing;
        Result.Progress = 0.0f;
    }

    return Result;
}

void FCortexGenFalProvider::PollJobStatus(
    const FString& ProviderJobId, FOnGenJobStatusReceived OnComplete)
{
    // Pre-build URLs and auth header by value so the lambda does not capture `this`
    // (the provider may be destroyed before the async callback fires).
    FString PollStatusUrl = StatusUrl(ProviderJobId);
    FString PollResultUrl = FetchResultUrl(ProviderJobId);
    FString AuthHeader = FString::Printf(TEXT("Key %s"), *ApiKey);

    TSharedRef<IHttpRequest> HttpRequest = CreateRequest(TEXT("GET"), PollStatusUrl);

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [PollResultUrl, AuthHeader, OnComplete](
            FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
        {
            FCortexGenPollResult Result;

            if (!bConnectedSuccessfully || !Response.IsValid())
            {
                Result.bSuccess = false;
                Result.ErrorMessage = TEXT("HTTP connection failed");
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            if (Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
            {
                Result.bSuccess = false;
                Result.ErrorMessage = FString::Printf(
                    TEXT("fal.ai poll error %d: %s"),
                    Response->GetResponseCode(),
                    *Response->GetContentAsString());
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            // Inline the poll response parse — no `this` capture needed (pure JSON logic)
            {
                TSharedPtr<FJsonObject> Json;
                TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
                    Response->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
                {
                    Result.bSuccess = false;
                    Result.ErrorMessage = TEXT("Failed to parse poll response");
                }
                else
                {
                    Result.bSuccess = true;
                    FString StatusStr = Json->GetStringField(TEXT("status"));
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
                    else
                    {
                        Result.Status = ECortexGenJobStatus::Processing;
                        Result.Progress = 0.0f;
                    }
                }
            }

            if (Result.bSuccess && Result.Status == ECortexGenJobStatus::Complete)
            {
                TSharedRef<IHttpRequest> ResultRequest = FHttpModule::Get().CreateRequest();
                ResultRequest->SetVerb(TEXT("GET"));
                ResultRequest->SetURL(PollResultUrl);
                ResultRequest->SetHeader(TEXT("Authorization"), AuthHeader);
                ResultRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

                ResultRequest->OnProcessRequestComplete().BindLambda(
                    [Result, OnComplete](
                        FHttpRequestPtr, FHttpResponsePtr ResResponse,
                        bool bResConnected) mutable
                    {
                        if (bResConnected && ResResponse.IsValid() &&
                            ResResponse->GetResponseCode() >= 200 &&
                            ResResponse->GetResponseCode() < 300)
                        {
                            TSharedPtr<FJsonObject> ResJson;
                            TSharedRef<TJsonReader<>> ResReader =
                                TJsonReaderFactory<>::Create(ResResponse->GetContentAsString());
                            if (FJsonSerializer::Deserialize(ResReader, ResJson) &&
                                ResJson.IsValid())
                            {
                                const TSharedPtr<FJsonObject>* OutputObj = nullptr;
                                if (ResJson->TryGetObjectField(TEXT("output"), OutputObj) &&
                                    OutputObj->IsValid())
                                {
                                    const TSharedPtr<FJsonObject>* MeshObj = nullptr;
                                    if ((*OutputObj)->TryGetObjectField(
                                            TEXT("model_mesh"), MeshObj) &&
                                        MeshObj->IsValid())
                                    {
                                        Result.ResultUrl =
                                            (*MeshObj)->GetStringField(TEXT("url"));
                                    }
                                }
                            }
                        }
                        OnComplete.ExecuteIfBound(Result);
                    });

                ResultRequest->ProcessRequest();
                return;
            }

            OnComplete.ExecuteIfBound(Result);
        });

    HttpRequest->ProcessRequest();
}

void FCortexGenFalProvider::CancelJob(
    const FString& ProviderJobId, FOnGenJobCancelled OnComplete)
{
    TSharedRef<IHttpRequest> HttpRequest = CreateRequest(TEXT("PUT"), CancelUrl(ProviderJobId));

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
        {
            bool bSuccess = bConnectedSuccessfully && Response.IsValid() &&
                Response->GetResponseCode() >= 200 && Response->GetResponseCode() < 300;
            OnComplete.ExecuteIfBound(bSuccess);
        });

    HttpRequest->ProcessRequest();
}

void FCortexGenFalProvider::DownloadResult(
    const FString& ResultUrl, const FString& LocalPath, FOnGenDownloadComplete OnComplete)
{
    TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
    HttpRequest->SetVerb(TEXT("GET"));
    HttpRequest->SetURL(ResultUrl);

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [LocalPath, OnComplete](
            FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
        {
            if (!bConnectedSuccessfully || !Response.IsValid() ||
                Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
            {
                OnComplete.ExecuteIfBound(false, TEXT("Download failed"));
                return;
            }

            if (!FFileHelper::SaveArrayToFile(Response->GetContent(), *LocalPath))
            {
                OnComplete.ExecuteIfBound(false, FString::Printf(
                    TEXT("Failed to save file to %s"), *LocalPath));
                return;
            }

            OnComplete.ExecuteIfBound(true, FString());
        });

    HttpRequest->ProcessRequest();
}
