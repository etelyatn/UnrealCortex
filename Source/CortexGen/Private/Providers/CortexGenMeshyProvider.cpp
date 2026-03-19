#include "Providers/CortexGenMeshyProvider.h"
#include "CortexGenModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

FCortexGenMeshyProvider::FCortexGenMeshyProvider(const FString& InApiKey)
    : ApiKey(InApiKey)
{
}

FName FCortexGenMeshyProvider::GetProviderId() const
{
    return FName(TEXT("meshy"));
}

FText FCortexGenMeshyProvider::GetDisplayName() const
{
    return FText::FromString(TEXT("Meshy"));
}

ECortexGenCapability FCortexGenMeshyProvider::GetCapabilities() const
{
    return ECortexGenCapability::MeshFromText
        | ECortexGenCapability::MeshFromImage
        | ECortexGenCapability::Texturing;
}

FString FCortexGenMeshyProvider::BaseUrl()
{
    return TEXT("https://api.meshy.ai/openapi/v2");
}

TSharedRef<IHttpRequest> FCortexGenMeshyProvider::CreateRequest(
    const FString& Verb, const FString& Endpoint) const
{
    TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
    Request->SetVerb(Verb);
    Request->SetURL(BaseUrl() + Endpoint);
    Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    return Request;
}

void FCortexGenMeshyProvider::SubmitJob(
    const FCortexGenJobRequest& Request, FOnGenJobSubmitted OnComplete)
{
    TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("mode"), TEXT("preview"));
    Body->SetStringField(TEXT("prompt"), Request.Prompt);
    Body->SetStringField(TEXT("output_format"), TEXT("glb"));

    // Override with provider-specific params if present
    for (const auto& Pair : Request.Params)
    {
        Body->SetStringField(Pair.Key, Pair.Value);
    }

    FString SubmitEndpoint;
    switch (Request.Type)
    {
    case ECortexGenJobType::MeshFromText:
    case ECortexGenJobType::MeshFromBoth:
        SubmitEndpoint = TEXT("text-to-3d");
        break;
    case ECortexGenJobType::MeshFromImage:
        SubmitEndpoint = TEXT("image-to-3d");
        break;
    case ECortexGenJobType::Texturing:
        SubmitEndpoint = TEXT("text-to-texture");
        if (!Request.SourceModelPath.IsEmpty())
        {
            Body->SetStringField(TEXT("model_url"), Request.SourceModelPath);
        }
        break;
    default:
        FCortexGenSubmitResult Result;
        Result.bSuccess = false;
        Result.ErrorMessage = TEXT("Unsupported job type for Meshy provider");
        OnComplete.ExecuteIfBound(Result);
        return;
    }

    TSharedRef<IHttpRequest> HttpRequest = CreateRequest(TEXT("POST"), TEXT("/") + SubmitEndpoint);

    FString BodyString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyString);
    FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
    HttpRequest->SetContentAsString(BodyString);

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [OnComplete, SubmitEndpoint](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
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
                    TEXT("Meshy API error %d: %s"),
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
                Result.ErrorMessage = TEXT("Failed to parse Meshy API response");
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            Result.bSuccess = true;
            FString ActualJobId = Json->GetStringField(TEXT("result"));
            Result.ProviderJobId = FString::Printf(TEXT("%s|%s"), *SubmitEndpoint, *ActualJobId);
            OnComplete.ExecuteIfBound(Result);
        });

    HttpRequest->ProcessRequest();
}

void FCortexGenMeshyProvider::PollJobStatus(
    const FString& ProviderJobId, FOnGenJobStatusReceived OnComplete)
{
    FString EndpointPrefix;
    FString ActualJobId;
    if (!ProviderJobId.Split(TEXT("|"), &EndpointPrefix, &ActualJobId))
    {
        // Backwards compatibility: no prefix encoded, assume text-to-3d
        EndpointPrefix = TEXT("text-to-3d");
        ActualJobId = ProviderJobId;
    }

    FString Endpoint = FString::Printf(TEXT("/%s/%s"), *EndpointPrefix, *ActualJobId);
    TSharedRef<IHttpRequest> HttpRequest = CreateRequest(TEXT("GET"), Endpoint);

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
        {
            FCortexGenPollResult Result;

            if (!bConnectedSuccessfully || !Response.IsValid())
            {
                Result.bSuccess = false;
                Result.ErrorMessage = TEXT("HTTP connection failed");
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            TSharedPtr<FJsonObject> Json;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
                Response->GetContentAsString());
            if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
            {
                Result.bSuccess = false;
                Result.ErrorMessage = TEXT("Failed to parse poll response");
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            Result.bSuccess = true;
            FString StatusStr = Json->GetStringField(TEXT("status"));

            if (StatusStr == TEXT("SUCCEEDED"))
            {
                Result.Status = ECortexGenJobStatus::Complete;
                Result.Progress = 1.0f;
                // Extract model URL from response
                const TSharedPtr<FJsonObject>* ModelUrls = nullptr;
                if (Json->TryGetObjectField(TEXT("model_urls"), ModelUrls) && ModelUrls->IsValid())
                {
                    Result.ResultUrl = (*ModelUrls)->GetStringField(TEXT("glb"));
                }
            }
            else if (StatusStr == TEXT("FAILED") || StatusStr == TEXT("EXPIRED"))
            {
                Result.Status = ECortexGenJobStatus::Failed;
                Result.ErrorMessage = Json->GetStringField(TEXT("task_error"));
            }
            else // IN_PROGRESS, PENDING
            {
                Result.Status = ECortexGenJobStatus::Processing;
                Result.Progress = static_cast<float>(
                    Json->GetNumberField(TEXT("progress"))) / 100.0f;
            }

            OnComplete.ExecuteIfBound(Result);
        });

    HttpRequest->ProcessRequest();
}

void FCortexGenMeshyProvider::CancelJob(
    const FString& ProviderJobId, FOnGenJobCancelled OnComplete)
{
    // Meshy API does not support job cancellation.
    // Local job state is transitioned to Cancelled by FCortexGenJobManager.
    UE_LOG(LogCortexGen, Log,
        TEXT("CancelJob: provider-side cancellation not supported for Meshy (job %s). Local state will be marked cancelled."),
        *ProviderJobId);
    OnComplete.ExecuteIfBound(true);
}

void FCortexGenMeshyProvider::DownloadResult(
    const FString& ResultUrl, const FString& LocalPath, FOnGenDownloadComplete OnComplete)
{
    TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
    HttpRequest->SetVerb(TEXT("GET"));
    HttpRequest->SetURL(ResultUrl);

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [LocalPath, OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
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
