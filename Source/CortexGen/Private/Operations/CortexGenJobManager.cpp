#include "Operations/CortexGenJobManager.h"
#include "CortexGenModule.h"
#include "CortexGenSettings.h"
#include "Providers/ICortexGenProvider.h"
#include "Operations/CortexGenAssetImporter.h"
#include "CortexCoreModule.h"
#include "Dom/JsonObject.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Containers/Ticker.h"

FCortexGenJobManager::FCortexGenJobManager()
{
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    if (Settings)
    {
        MaxConcurrentJobs = Settings->MaxConcurrentJobs;
    }

    // Note: LoadJobs() is called explicitly by FCortexGenModule::StartupModule()
    // so that unit-test instances start with clean state.

    // Set up polling ticker
    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([this](float /*DeltaTime*/)
        {
            PollActiveJobs();
            return true;
        }),
        Settings ? static_cast<float>(Settings->PollIntervalSeconds) : 5.0f
    );
}

FCortexGenJobManager::~FCortexGenJobManager()
{
    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
    }
    SaveJobs();
}

void FCortexGenJobManager::RegisterProvider(TSharedPtr<ICortexGenProvider> Provider)
{
    if (Provider.IsValid())
    {
        Providers.Add(Provider->GetProviderId().ToString(), Provider);
        UE_LOG(LogCortexGen, Log, TEXT("Registered provider: %s"),
            *Provider->GetProviderId().ToString());
    }
}

TSharedPtr<ICortexGenProvider> FCortexGenJobManager::GetProvider(const FString& ProviderId) const
{
    const TSharedPtr<ICortexGenProvider>* Found = Providers.Find(ProviderId);
    return Found ? *Found : nullptr;
}

TArray<TSharedPtr<ICortexGenProvider>> FCortexGenJobManager::GetProviders() const
{
    TArray<TSharedPtr<ICortexGenProvider>> Result;
    for (const auto& Pair : Providers)
    {
        Result.Add(Pair.Value);
    }
    return Result;
}

FString FCortexGenJobManager::GenerateJobId()
{
    FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Short);
    return FString::Printf(TEXT("gen_%s"), *Guid.Left(8).ToLower());
}

ECortexGenCapability FCortexGenJobManager::JobTypeToCapability(ECortexGenJobType Type)
{
    switch (Type)
    {
    case ECortexGenJobType::MeshFromText:
    case ECortexGenJobType::MeshFromBoth:
        return ECortexGenCapability::MeshFromText;
    case ECortexGenJobType::MeshFromImage:
        return ECortexGenCapability::MeshFromImage;
    case ECortexGenJobType::ImageFromText:
        return ECortexGenCapability::ImageFromText;
    case ECortexGenJobType::Texturing:
        return ECortexGenCapability::Texturing;
    default:
        return ECortexGenCapability::None;
    }
}

int32 FCortexGenJobManager::CountActiveJobs() const
{
    int32 Count = 0;
    for (const auto& Pair : Jobs)
    {
        ECortexGenJobStatus Status = Pair.Value.Status;
        if (Status == ECortexGenJobStatus::Pending ||
            Status == ECortexGenJobStatus::Processing ||
            Status == ECortexGenJobStatus::Downloading ||
            Status == ECortexGenJobStatus::Importing)
        {
            Count++;
        }
    }
    return Count;
}

bool FCortexGenJobManager::SubmitJob(const FString& ProviderId,
    const FCortexGenJobRequest& Request, FString& OutJobId, FString& OutError)
{
    // Validate provider exists
    TSharedPtr<ICortexGenProvider> Provider = GetProvider(ProviderId);
    if (!Provider.IsValid())
    {
        OutError = FString::Printf(
            TEXT("Provider '%s' not found. Available providers: "), *ProviderId);
        for (const auto& Pair : Providers)
        {
            OutError += Pair.Key + TEXT(" ");
        }
        return false;
    }

    // Validate capability
    ECortexGenCapability RequiredCap = JobTypeToCapability(Request.Type);
    if (!EnumHasAnyFlags(Provider->GetCapabilities(), RequiredCap))
    {
        OutError = FString::Printf(
            TEXT("Provider '%s' does not support the requested job type"),
            *ProviderId);
        return false;
    }

    // Check concurrent limit
    if (CountActiveJobs() >= MaxConcurrentJobs)
    {
        OutError = FString::Printf(
            TEXT("Maximum concurrent jobs (%d) reached. Wait for a job to complete or increase the limit in settings."),
            MaxConcurrentJobs);
        return false;
    }

    // Create job state
    FString JobId = GenerateJobId();
    FCortexGenJobState& Job = Jobs.Add(JobId);
    Job.JobId = JobId;
    Job.Type = Request.Type;
    Job.Provider = ProviderId;
    Job.Status = ECortexGenJobStatus::Pending;
    Job.Prompt = Request.Prompt;
    Job.Destination = Request.Destination;
    Job.CreatedAt = FDateTime::UtcNow().ToIso8601();

    OutJobId = JobId;

    // Submit to provider (may be async)
    Provider->SubmitJob(Request, FOnGenJobSubmitted::CreateLambda(
        [this, JobId](const FCortexGenSubmitResult& Result)
        {
            FCortexGenJobState* JobPtr = Jobs.Find(JobId);
            if (!JobPtr)
            {
                return;
            }

            if (Result.bSuccess)
            {
                JobPtr->ProviderJobId = Result.ProviderJobId;
                TransitionJob(*JobPtr, ECortexGenJobStatus::Processing);
            }
            else
            {
                JobPtr->ErrorMessage = Result.ErrorMessage;
                TransitionJob(*JobPtr, ECortexGenJobStatus::Failed);
            }
        }));

    return true;
}

bool FCortexGenJobManager::CancelJob(const FString& JobId, FString& OutError)
{
    FCortexGenJobState* Job = Jobs.Find(JobId);
    if (!Job)
    {
        OutError = FString::Printf(TEXT("Job '%s' not found"), *JobId);
        return false;
    }

    if (Job->Status == ECortexGenJobStatus::Cancelled ||
        Job->Status == ECortexGenJobStatus::Imported ||
        Job->Status == ECortexGenJobStatus::Failed)
    {
        OutError = FString::Printf(TEXT("Job '%s' is already in terminal state"), *JobId);
        return false;
    }

    // Notify provider (best effort)
    TSharedPtr<ICortexGenProvider> Provider = GetProvider(Job->Provider);
    if (Provider.IsValid())
    {
        Provider->CancelJob(Job->ProviderJobId, FOnGenJobCancelled::CreateLambda(
            [](bool) {}));
    }

    TransitionJob(*Job, ECortexGenJobStatus::Cancelled);
    return true;
}

bool FCortexGenJobManager::DeleteJob(const FString& JobId, FString& OutError)
{
    const FCortexGenJobState* Job = Jobs.Find(JobId);
    if (!Job)
    {
        OutError = FString::Printf(TEXT("Job '%s' not found"), *JobId);
        return false;
    }

    // Cannot delete active jobs
    if (Job->Status == ECortexGenJobStatus::Pending ||
        Job->Status == ECortexGenJobStatus::Processing ||
        Job->Status == ECortexGenJobStatus::Downloading ||
        Job->Status == ECortexGenJobStatus::Importing)
    {
        OutError = FString::Printf(TEXT("Cannot delete active job '%s'. Cancel it first."), *JobId);
        return false;
    }

    Jobs.Remove(JobId);
    SaveJobs();
    return true;
}

bool FCortexGenJobManager::RetryImport(const FString& JobId, FString& OutError)
{
    FCortexGenJobState* Job = Jobs.Find(JobId);
    if (!Job)
    {
        OutError = FString::Printf(TEXT("Job '%s' not found"), *JobId);
        return false;
    }

    if (Job->Status == ECortexGenJobStatus::DownloadFailed)
    {
        StartDownloadPipeline(*Job);
        return true;
    }

    if (Job->Status == ECortexGenJobStatus::ImportFailed)
    {
        TransitionJob(*Job, ECortexGenJobStatus::Importing);
        RunImportPipeline(*Job);
        return true;
    }

    OutError = FString::Printf(
        TEXT("Job '%s' is not in a retryable state"), *JobId);
    return false;
}

const FCortexGenJobState* FCortexGenJobManager::GetJobState(const FString& JobId) const
{
    return Jobs.Find(JobId);
}

TArray<FCortexGenJobState> FCortexGenJobManager::ListJobs(
    const FString& StatusFilter, int32 Limit) const
{
    TArray<FCortexGenJobState> Result;
    for (const auto& Pair : Jobs)
    {
        if (!StatusFilter.IsEmpty())
        {
            FString StatusStr = StaticEnum<ECortexGenJobStatus>()->GetNameStringByValue(
                static_cast<int64>(Pair.Value.Status));
            if (!StatusStr.Equals(StatusFilter, ESearchCase::IgnoreCase))
            {
                continue;
            }
        }
        Result.Add(Pair.Value);
    }

    // Sort by creation date (newest first)
    Result.Sort([](const FCortexGenJobState& A, const FCortexGenJobState& B)
    {
        return A.CreatedAt > B.CreatedAt;
    });

    if (Limit > 0 && Result.Num() > Limit)
    {
        Result.SetNum(Limit);
    }

    return Result;
}

void FCortexGenJobManager::SetMaxConcurrentJobs(int32 Max)
{
    MaxConcurrentJobs = FMath::Clamp(Max, 1, 10);
}

void FCortexGenJobManager::TransitionJob(
    FCortexGenJobState& Job, ECortexGenJobStatus NewStatus)
{
    ECortexGenJobStatus OldStatus = Job.Status;
    Job.Status = NewStatus;

    // Set completion time for terminal states
    if (NewStatus == ECortexGenJobStatus::Imported ||
        NewStatus == ECortexGenJobStatus::Failed ||
        NewStatus == ECortexGenJobStatus::Cancelled)
    {
        Job.CompletedAt = FDateTime::UtcNow().ToIso8601();
    }

    UE_LOG(LogCortexGen, Log, TEXT("Job %s: %s -> %s"),
        *Job.JobId,
        *StaticEnum<ECortexGenJobStatus>()->GetNameStringByValue(static_cast<int64>(OldStatus)),
        *StaticEnum<ECortexGenJobStatus>()->GetNameStringByValue(static_cast<int64>(NewStatus)));

    JobStateChangedDelegate.Broadcast(Job);
    BroadcastJobProgress(Job);
    SaveJobs();
}

void FCortexGenJobManager::PollActiveJobs()
{
    for (auto& Pair : Jobs)
    {
        FCortexGenJobState& Job = Pair.Value;

        if (Job.Status != ECortexGenJobStatus::Processing)
        {
            continue;
        }

        TSharedPtr<ICortexGenProvider> Provider = GetProvider(Job.Provider);
        if (!Provider.IsValid())
        {
            continue;
        }

        FString JobId = Job.JobId;
        Provider->PollJobStatus(Job.ProviderJobId,
            FOnGenJobStatusReceived::CreateLambda(
                [this, JobId](const FCortexGenPollResult& Result)
                {
                    FCortexGenJobState* Job = Jobs.Find(JobId);
                    if (!Job || Job->Status != ECortexGenJobStatus::Processing)
                    {
                        return;
                    }

                    if (!Result.bSuccess)
                    {
                        Job->ErrorMessage = Result.ErrorMessage;
                        TransitionJob(*Job, ECortexGenJobStatus::Failed);
                        return;
                    }

                    Job->Progress = Result.Progress;

                    if (Result.Status == ECortexGenJobStatus::Complete)
                    {
                        Job->ResultUrl = Result.ResultUrl;
                        TransitionJob(*Job, ECortexGenJobStatus::Complete);
                        StartDownloadPipeline(*Job);
                    }
                    else if (Result.Status == ECortexGenJobStatus::Failed)
                    {
                        Job->ErrorMessage = Result.ErrorMessage;
                        TransitionJob(*Job, ECortexGenJobStatus::Failed);
                    }
                    else
                    {
                        // Still processing — broadcast progress update
                        JobStateChangedDelegate.Broadcast(*Job);
                        BroadcastJobProgress(*Job);
                    }
                }));
    }
}

void FCortexGenJobManager::StartDownloadPipeline(FCortexGenJobState& Job)
{
    if (Job.ResultUrl.IsEmpty())
    {
        Job.ErrorMessage = TEXT("No download URL available");
        TransitionJob(Job, ECortexGenJobStatus::DownloadFailed);
        return;
    }

    TransitionJob(Job, ECortexGenJobStatus::Downloading);

    // Determine download path
    FString DownloadDir = FPaths::ProjectSavedDir() / TEXT("CortexGen") / TEXT("downloads");
    IPlatformFile::GetPlatformPhysical().CreateDirectoryTree(*DownloadDir);
    FString LocalPath = DownloadDir / FString::Printf(TEXT("%s.glb"), *Job.JobId);

    TSharedPtr<ICortexGenProvider> Provider = GetProvider(Job.Provider);
    if (!Provider.IsValid())
    {
        Job.ErrorMessage = TEXT("Provider no longer available");
        TransitionJob(Job, ECortexGenJobStatus::DownloadFailed);
        return;
    }

    FString JobId = Job.JobId;
    Provider->DownloadResult(Job.ResultUrl, LocalPath,
        FOnGenDownloadComplete::CreateLambda(
            [this, JobId, LocalPath](bool bSuccess, const FString& ErrorMsg)
            {
                FCortexGenJobState* Job = Jobs.Find(JobId);
                if (!Job) return;

                if (bSuccess)
                {
                    Job->DownloadPath = LocalPath;
                    TransitionJob(*Job, ECortexGenJobStatus::Importing);
                    RunImportPipeline(*Job);
                }
                else
                {
                    Job->ErrorMessage = ErrorMsg;
                    TransitionJob(*Job, ECortexGenJobStatus::DownloadFailed);
                }
            }));
}

void FCortexGenJobManager::RunImportPipeline(FCortexGenJobState& Job)
{
    if (Job.DownloadPath.IsEmpty())
    {
        Job.ErrorMessage = TEXT("No downloaded file available for import");
        TransitionJob(Job, ECortexGenJobStatus::ImportFailed);
        return;
    }

    // Determine destination path and asset name
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    FString Destination = Job.Destination.IsEmpty()
        ? Settings->DefaultMeshDestination
        : Job.Destination;

    // Use job ID as asset name (unique, deterministic)
    FString AssetName = Job.JobId;

    FCortexGenAssetImporter::FImportResult ImportResult =
        FCortexGenAssetImporter::ImportAsset(Job.DownloadPath, Destination, AssetName);

    if (ImportResult.bSuccess)
    {
        Job.ImportedAssetPaths = ImportResult.ImportedAssetPaths;
        TransitionJob(Job, ECortexGenJobStatus::Imported);
    }
    else
    {
        Job.ErrorMessage = ImportResult.ErrorMessage;
        TransitionJob(Job, ECortexGenJobStatus::ImportFailed);
    }
}

void FCortexGenJobManager::BroadcastJobProgress(const FCortexGenJobState& Job)
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
    {
        return;
    }

    TSharedPtr<FJsonObject> ProgressData = MakeShared<FJsonObject>();
    ProgressData->SetStringField(TEXT("job_id"), Job.JobId);
    ProgressData->SetStringField(TEXT("status"),
        StaticEnum<ECortexGenJobStatus>()->GetNameStringByValue(
            static_cast<int64>(Job.Status)).ToLower());
    ProgressData->SetNumberField(TEXT("progress"), Job.Progress);
    ProgressData->SetStringField(TEXT("prompt"), Job.Prompt);
    if (!Job.ErrorMessage.IsEmpty())
    {
        ProgressData->SetStringField(TEXT("error"), Job.ErrorMessage);
    }
    TArray<TSharedPtr<FJsonValue>> PathsArray;
    for (const FString& Path : Job.ImportedAssetPaths)
    {
        PathsArray.Add(MakeShared<FJsonValueString>(Path));
    }
    ProgressData->SetArrayField(TEXT("asset_paths"), PathsArray);

    FCortexCoreModule& CoreModule =
        FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    CoreModule.OnDomainProgress().Broadcast(FName(TEXT("gen")), ProgressData);
}

void FCortexGenJobManager::SaveJobs() const
{
    // Do not write to disk unless LoadJobs() has been called (guards test isolation).
    if (!bPersistenceEnabled)
    {
        return;
    }

    FString SaveDir = FPaths::ProjectSavedDir() / TEXT("CortexGen");
    IPlatformFile::GetPlatformPhysical().CreateDirectoryTree(*SaveDir);

    TArray<TSharedPtr<FJsonValue>> JobArray;
    for (const auto& Pair : Jobs)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (FJsonObjectConverter::UStructToJsonObject(
            FCortexGenJobState::StaticStruct(), &Pair.Value, Obj.ToSharedRef(), 0, 0))
        {
            JobArray.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetArrayField(TEXT("jobs"), JobArray);

    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

    // Atomic write: write to temp, then rename
    FString TempPath = SaveDir / TEXT("Jobs.json.tmp");
    FString FinalPath = SaveDir / TEXT("Jobs.json");

    if (FFileHelper::SaveStringToFile(JsonString, *TempPath))
    {
        IPlatformFile::GetPlatformPhysical().MoveFile(*FinalPath, *TempPath);
    }
}

void FCortexGenJobManager::LoadJobs()
{
    bPersistenceEnabled = true;

    FString FilePath = FPaths::ProjectSavedDir() / TEXT("CortexGen") / TEXT("Jobs.json");
    FString JsonString;

    if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
    {
        return;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        UE_LOG(LogCortexGen, Warning, TEXT("Failed to parse Jobs.json"));
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* JobArray = nullptr;
    if (!Root->TryGetArrayField(TEXT("jobs"), JobArray))
    {
        return;
    }

    for (const auto& Value : *JobArray)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Value->TryGetObject(Obj) || !Obj->IsValid())
        {
            continue;
        }

        FCortexGenJobState State;
        if (!FJsonObjectConverter::JsonObjectToUStruct(
            (*Obj).ToSharedRef(), &State))
        {
            continue;
        }

        // Active jobs cannot be resumed after a restart (provider connection is gone).
        // Mark them as failed so they don't count toward concurrency limits.
        if (State.Status == ECortexGenJobStatus::Pending ||
            State.Status == ECortexGenJobStatus::Processing ||
            State.Status == ECortexGenJobStatus::Downloading ||
            State.Status == ECortexGenJobStatus::Importing)
        {
            State.Status = ECortexGenJobStatus::Failed;
            State.ErrorMessage = TEXT("Job interrupted by editor restart");
            State.CompletedAt = FDateTime::UtcNow().ToIso8601();
        }

        Jobs.Add(State.JobId, State);
    }

    UE_LOG(LogCortexGen, Log, TEXT("Loaded %d jobs from persistence"), Jobs.Num());
}
