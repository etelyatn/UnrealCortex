#include "Providers/CortexGenTripoProvider.h"
#include "CortexGenModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

FCortexGenTripoProvider::FCortexGenTripoProvider(const FString& InApiKey)
    : ApiKey(InApiKey)
{
}

FName FCortexGenTripoProvider::GetProviderId() const
{
    return FName(TEXT("tripo3d"));
}

FText FCortexGenTripoProvider::GetDisplayName() const
{
    return FText::FromString(TEXT("Tripo3D"));
}

ECortexGenCapability FCortexGenTripoProvider::GetCapabilities() const
{
    return ECortexGenCapability::MeshFromText | ECortexGenCapability::MeshFromImage;
}

FString FCortexGenTripoProvider::BaseUrl()
{
    return TEXT("https://api.tripo3d.ai/v2/openapi");
}

TSharedRef<IHttpRequest> FCortexGenTripoProvider::CreateRequest(
    const FString& Verb, const FString& Endpoint) const
{
    TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
    Request->SetVerb(Verb);
    Request->SetURL(BaseUrl() + Endpoint);
    Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    return Request;
}

void FCortexGenTripoProvider::SubmitJob(
    const FCortexGenJobRequest& Request, FOnGenJobSubmitted OnComplete)
{
    if (Request.Type == ECortexGenJobType::Texturing ||
        Request.Type == ECortexGenJobType::ImageFromText)
    {
        FCortexGenSubmitResult Result;
        Result.bSuccess = false;
        Result.ErrorMessage = TEXT("Unsupported job type for Tripo3D provider");
        OnComplete.ExecuteIfBound(Result);
        return;
    }

    TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("type"), TEXT("text_to_model"));
    Body->SetStringField(TEXT("prompt"), Request.Prompt);
    Body->SetStringField(TEXT("model_version"), TEXT("v2.0-20240919"));
    Body->SetStringField(TEXT("output_format"), TEXT("glb"));

    if (Request.Type == ECortexGenJobType::MeshFromImage ||
        Request.Type == ECortexGenJobType::MeshFromBoth)
    {
        Body->SetStringField(TEXT("type"), TEXT("image_to_model"));
    }

    // Override with provider-specific params
    for (const auto& Pair : Request.Params)
    {
        Body->SetStringField(Pair.Key, Pair.Value);
    }

    TSharedRef<IHttpRequest> HttpRequest = CreateRequest(TEXT("POST"), TEXT("/task"));

    FString BodyString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyString);
    FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
    HttpRequest->SetContentAsString(BodyString);

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

            TSharedPtr<FJsonObject> Json;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
                Response->GetContentAsString());
            if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
            {
                Result.bSuccess = false;
                Result.ErrorMessage = TEXT("Failed to parse Tripo3D response");
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            int32 Code = static_cast<int32>(Json->GetNumberField(TEXT("code")));
            if (Code != 0)
            {
                Result.bSuccess = false;
                Result.ErrorMessage = Json->GetStringField(TEXT("message"));
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            const TSharedPtr<FJsonObject>* DataObj = nullptr;
            if (Json->TryGetObjectField(TEXT("data"), DataObj) && DataObj->IsValid())
            {
                Result.bSuccess = true;
                Result.ProviderJobId = (*DataObj)->GetStringField(TEXT("task_id"));
            }
            else
            {
                Result.bSuccess = false;
                Result.ErrorMessage = TEXT("Missing data in Tripo3D response");
            }

            OnComplete.ExecuteIfBound(Result);
        });

    HttpRequest->ProcessRequest();
}

void FCortexGenTripoProvider::PollJobStatus(
    const FString& ProviderJobId, FOnGenJobStatusReceived OnComplete)
{
    FString Endpoint = FString::Printf(TEXT("/task/%s"), *ProviderJobId);
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

            const TSharedPtr<FJsonObject>* DataObj = nullptr;
            if (!Json->TryGetObjectField(TEXT("data"), DataObj) || !DataObj->IsValid())
            {
                Result.bSuccess = false;
                Result.ErrorMessage = TEXT("Missing data in poll response");
                OnComplete.ExecuteIfBound(Result);
                return;
            }

            Result.bSuccess = true;
            FString StatusStr = (*DataObj)->GetStringField(TEXT("status"));

            if (StatusStr == TEXT("success"))
            {
                Result.Status = ECortexGenJobStatus::Complete;
                Result.Progress = 1.0f;
                const TSharedPtr<FJsonObject>* OutputObj = nullptr;
                if ((*DataObj)->TryGetObjectField(TEXT("output"), OutputObj) && OutputObj->IsValid())
                {
                    Result.ResultUrl = (*OutputObj)->GetStringField(TEXT("model"));
                }
            }
            else if (StatusStr == TEXT("failed") || StatusStr == TEXT("cancelled") ||
                     StatusStr == TEXT("unknown"))
            {
                Result.Status = ECortexGenJobStatus::Failed;
                Result.ErrorMessage = (*DataObj)->GetStringField(TEXT("message"));
            }
            else // running, queued
            {
                Result.Status = ECortexGenJobStatus::Processing;
                Result.Progress = static_cast<float>(
                    (*DataObj)->GetNumberField(TEXT("progress"))) / 100.0f;
            }

            OnComplete.ExecuteIfBound(Result);
        });

    HttpRequest->ProcessRequest();
}

void FCortexGenTripoProvider::CancelJob(
    const FString& ProviderJobId, FOnGenJobCancelled OnComplete)
{
    // Tripo3D doesn't expose a cancel endpoint — mark locally
    OnComplete.ExecuteIfBound(true);
}

void FCortexGenTripoProvider::DownloadResult(
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
