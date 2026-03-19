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
#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"

namespace
{
/** Execute a task on the Game Thread. If already on the Game Thread, runs inline. */
void RunOnGameThread(TFunction<void()> Task)
{
    if (IsInGameThread())
    {
        Task();
    }
    else
    {
        AsyncTask(ENamedThreads::GameThread, MoveTemp(Task));
    }
}
} // anonymous namespace

FCortexGenJobManager::FCortexGenJobManager()
{
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    if (Settings)
    {
        MaxConcurrentJobs = Settings->MaxConcurrentJobs;
    }

    // Note: Initialize() must be called after construction to start the ticker.
    // This is separate because TSharedFromThis requires the object to be held
    // by a TSharedPtr before AsShared() can be called.
}

void FCortexGenJobManager::Initialize()
{
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();

    LoadTimingData();

    // Use TWeakPtr to prevent the ticker from preventing destruction.
    TWeakPtr<FCortexGenJobManager> WeakSelf = AsShared();

    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakSelf](float /*DeltaTime*/)
        {
            TSharedPtr<FCortexGenJobManager> Self = WeakSelf.Pin();
            if (!Self.IsValid())
            {
                return false; // Stop ticker — manager destroyed
            }
            Self->PollActiveJobs();
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
        return ECortexGenCapability::MeshFromText;
    case ECortexGenJobType::MeshFromBoth:
        // MeshFromBoth requires both text and image capabilities
        return ECortexGenCapability::MeshFromText | ECortexGenCapability::MeshFromImage;
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
    const FCortexGenJobRequest& Request, FString& OutJobId, FString& OutError,
    ECortexGenError& OutErrorCode)
{
    OutErrorCode = ECortexGenError::None;

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
        OutErrorCode = ECortexGenError::ProviderNotFound;
        return false;
    }

    // Validate capability
    ECortexGenCapability RequiredCap = JobTypeToCapability(Request.Type);
    if (!EnumHasAllFlags(Provider->GetCapabilities(), RequiredCap))
    {
        OutError = FString::Printf(
            TEXT("Provider '%s' does not support the requested job type"),
            *ProviderId);
        OutErrorCode = ECortexGenError::CapabilityNotSupported;
        return false;
    }

    // Check concurrent limit
    if (CountActiveJobs() >= MaxConcurrentJobs)
    {
        OutError = FString::Printf(
            TEXT("Maximum concurrent jobs (%d) reached. Wait for a job to complete or increase the limit in settings."),
            MaxConcurrentJobs);
        OutErrorCode = ECortexGenError::JobLimitReached;
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
    Job.ModelId = Request.ModelId;
    Job.CreatedAt = FDateTime::UtcNow().ToIso8601();

    OutJobId = JobId;

    UE_LOG(LogCortexGen, Log, TEXT("Submitting job %s to provider '%s' (type=%d, prompt='%s')"),
        *JobId, *ProviderId, static_cast<int32>(Request.Type),
        *Request.Prompt.Left(80));

    // Submit to provider (callback fires on HTTP thread — dispatch back to Game Thread)
    TWeakPtr<FCortexGenJobManager> WeakSelf = AsShared();
    Provider->SubmitJob(Request, FOnGenJobSubmitted::CreateLambda(
        [WeakSelf, JobId](const FCortexGenSubmitResult& Result)
        {
            RunOnGameThread([WeakSelf, JobId, Result]()
            {
                TSharedPtr<FCortexGenJobManager> Self = WeakSelf.Pin();
                if (!Self.IsValid()) return;

                FCortexGenJobState* JobPtr = Self->Jobs.Find(JobId);
                if (!JobPtr) return;

                if (Result.bSuccess)
                {
                    JobPtr->ProviderJobId = Result.ProviderJobId;
                    Self->TransitionJob(*JobPtr, ECortexGenJobStatus::Processing);
                }
                else
                {
                    JobPtr->ErrorMessage = Result.ErrorMessage;
                    Self->TransitionJob(*JobPtr, ECortexGenJobStatus::Failed);
                }
            });
        }));

    return true;
}

bool FCortexGenJobManager::CancelJob(const FString& JobId, FString& OutError,
    ECortexGenError& OutErrorCode)
{
    OutErrorCode = ECortexGenError::None;

    FCortexGenJobState* Job = Jobs.Find(JobId);
    if (!Job)
    {
        OutError = FString::Printf(TEXT("Job '%s' not found"), *JobId);
        OutErrorCode = ECortexGenError::JobNotFound;
        return false;
    }

    if (Job->Status == ECortexGenJobStatus::Cancelled ||
        Job->Status == ECortexGenJobStatus::Imported ||
        Job->Status == ECortexGenJobStatus::Failed)
    {
        OutError = FString::Printf(TEXT("Job '%s' is already in terminal state"), *JobId);
        OutErrorCode = ECortexGenError::JobInTerminalState;
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

bool FCortexGenJobManager::DeleteJob(const FString& JobId, FString& OutError,
    ECortexGenError& OutErrorCode)
{
    OutErrorCode = ECortexGenError::None;

    const FCortexGenJobState* Job = Jobs.Find(JobId);
    if (!Job)
    {
        OutError = FString::Printf(TEXT("Job '%s' not found"), *JobId);
        OutErrorCode = ECortexGenError::JobNotFound;
        return false;
    }

    // Cannot delete active jobs
    if (Job->Status == ECortexGenJobStatus::Pending ||
        Job->Status == ECortexGenJobStatus::Processing ||
        Job->Status == ECortexGenJobStatus::Downloading ||
        Job->Status == ECortexGenJobStatus::Importing)
    {
        OutError = FString::Printf(TEXT("Cannot delete active job '%s'. Cancel it first."), *JobId);
        OutErrorCode = ECortexGenError::JobInTerminalState;
        return false;
    }

    Jobs.Remove(JobId);
    SaveJobs();
    return true;
}

bool FCortexGenJobManager::RetryImport(const FString& JobId, FString& OutError,
    ECortexGenError& OutErrorCode)
{
    OutErrorCode = ECortexGenError::None;

    FCortexGenJobState* Job = Jobs.Find(JobId);
    if (!Job)
    {
        OutError = FString::Printf(TEXT("Job '%s' not found"), *JobId);
        OutErrorCode = ECortexGenError::JobNotFound;
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
    OutErrorCode = ECortexGenError::JobNotRetryable;
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

    if ((NewStatus == ECortexGenJobStatus::Failed ||
         NewStatus == ECortexGenJobStatus::DownloadFailed ||
         NewStatus == ECortexGenJobStatus::ImportFailed) &&
        !Job.ErrorMessage.IsEmpty())
    {
        UE_LOG(LogCortexGen, Warning, TEXT("Job %s error: %s"), *Job.JobId, *Job.ErrorMessage);
    }

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

        // Guard: skip if a poll is already in-flight for this job
        if (PollsInFlight.Contains(Job.JobId))
        {
            continue;
        }

        TSharedPtr<ICortexGenProvider> Provider = GetProvider(Job.Provider);
        if (!Provider.IsValid())
        {
            continue;
        }

        FString JobId = Job.JobId;
        PollsInFlight.Add(JobId);

        TWeakPtr<FCortexGenJobManager> WeakSelf = AsShared();
        Provider->PollJobStatus(Job.ProviderJobId,
            FOnGenJobStatusReceived::CreateLambda(
                [WeakSelf, JobId](const FCortexGenPollResult& Result)
                {
                    RunOnGameThread([WeakSelf, JobId, Result]()
                    {
                        TSharedPtr<FCortexGenJobManager> Self = WeakSelf.Pin();
                        if (!Self.IsValid()) return;

                        // Clear in-flight guard
                        Self->PollsInFlight.Remove(JobId);

                        // Re-lookup after async callback — the Jobs TMap reference from
                        // the outer scope is invalid across async boundaries (GEN-TD-07).
                        FCortexGenJobState* Job = Self->Jobs.Find(JobId);
                        if (!Job || Job->Status != ECortexGenJobStatus::Processing)
                        {
                            return;
                        }

                        if (!Result.bSuccess)
                        {
                            Job->ErrorMessage = Result.ErrorMessage;
                            Self->TransitionJob(*Job, ECortexGenJobStatus::Failed);
                            return;
                        }

                        Job->Progress = Result.Progress;

                        if (Result.Status == ECortexGenJobStatus::Complete)
                        {
                            Job->ResultUrl = Result.ResultUrl;
                            Self->TransitionJob(*Job, ECortexGenJobStatus::Complete);
                            Self->StartDownloadPipeline(*Job);
                        }
                        else if (Result.Status == ECortexGenJobStatus::Failed)
                        {
                            Job->ErrorMessage = Result.ErrorMessage;
                            Self->TransitionJob(*Job, ECortexGenJobStatus::Failed);
                        }
                        else
                        {
                            // Still processing — broadcast progress update
                            Self->JobStateChangedDelegate.Broadcast(*Job);
                            Self->BroadcastJobProgress(*Job);
                        }
                    });
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

    UE_LOG(LogCortexGen, Log, TEXT("Job %s: starting download from %s"), *Job.JobId, *Job.ResultUrl);
    TransitionJob(Job, ECortexGenJobStatus::Downloading);

    // Determine download path — images get .png, meshes get .glb
    FString DownloadDir = FPaths::ProjectSavedDir() / TEXT("CortexGen") / TEXT("downloads");
    IPlatformFile::GetPlatformPhysical().CreateDirectoryTree(*DownloadDir);
    FString Extension = (Job.Type == ECortexGenJobType::ImageFromText) ? TEXT("png") : TEXT("glb");
    FString LocalPath = DownloadDir / FString::Printf(TEXT("%s.%s"), *Job.JobId, *Extension);

    TSharedPtr<ICortexGenProvider> Provider = GetProvider(Job.Provider);
    if (!Provider.IsValid())
    {
        Job.ErrorMessage = TEXT("Provider no longer available");
        TransitionJob(Job, ECortexGenJobStatus::DownloadFailed);
        return;
    }

    FString JobId = Job.JobId;
    TWeakPtr<FCortexGenJobManager> WeakSelf = AsShared();
    Provider->DownloadResult(Job.ResultUrl, LocalPath,
        FOnGenDownloadComplete::CreateLambda(
            [WeakSelf, JobId, LocalPath](bool bSuccess, const FString& ErrorMsg)
            {
                RunOnGameThread([WeakSelf, JobId, LocalPath, bSuccess, ErrorMsg]()
                {
                    TSharedPtr<FCortexGenJobManager> Self = WeakSelf.Pin();
                    if (!Self.IsValid()) return;

                    // Re-lookup after async callback — the Jobs TMap reference from
                    // the outer scope is invalid across async boundaries (GEN-TD-07).
                    FCortexGenJobState* Job = Self->Jobs.Find(JobId);
                    if (!Job) return;

                    if (bSuccess)
                    {
                        Job->DownloadPath = LocalPath;
                        Self->TransitionJob(*Job, ECortexGenJobStatus::Importing);
                        Self->RunImportPipeline(*Job);
                    }
                    else
                    {
                        Job->ErrorMessage = ErrorMsg;
                        Self->TransitionJob(*Job, ECortexGenJobStatus::DownloadFailed);
                    }
                });
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
    FString Destination = Job.Destination;
    if (Destination.IsEmpty())
    {
        Destination = Settings
            ? Settings->DefaultMeshDestination
            : TEXT("/Game/Generated/Meshes");
    }

    // Use job ID as asset name (unique, deterministic)
    FString AssetName = Job.JobId;

    UE_LOG(LogCortexGen, Log, TEXT("Job %s: importing %s -> %s/%s"),
        *Job.JobId, *Job.DownloadPath, *Destination, *AssetName);

    FCortexGenAssetImporter::FImportResult ImportResult =
        FCortexGenAssetImporter::ImportAsset(Job.DownloadPath, Destination, AssetName);

    if (ImportResult.bSuccess)
    {
        Job.ImportedAssetPaths = ImportResult.ImportedAssetPaths;

        // Record timing from job creation to import completion
        FDateTime CreatedTime;
        if (FDateTime::ParseIso8601(*Job.CreatedAt, CreatedTime))
        {
            float Elapsed = static_cast<float>((FDateTime::UtcNow() - CreatedTime).GetTotalSeconds());
            if (!Job.ModelId.IsEmpty())
            {
                RecordTiming(Job.ModelId, Elapsed);
            }
        }

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

void FCortexGenJobManager::TrimJobHistory(int32 MaxHistory)
{
    if (Jobs.Num() <= MaxHistory)
    {
        return;
    }

    // Collect terminal jobs sorted by CreatedAt (oldest first)
    TArray<TPair<FString, FString>> TerminalJobs; // {JobId, CreatedAt}
    for (const auto& Pair : Jobs)
    {
        ECortexGenJobStatus Status = Pair.Value.Status;
        if (Status != ECortexGenJobStatus::Pending &&
            Status != ECortexGenJobStatus::Processing &&
            Status != ECortexGenJobStatus::Downloading &&
            Status != ECortexGenJobStatus::Importing)
        {
            TerminalJobs.Add({Pair.Key, Pair.Value.CreatedAt});
        }
    }

    TerminalJobs.Sort([](const TPair<FString, FString>& A,
        const TPair<FString, FString>& B)
    {
        return A.Value < B.Value;
    });

    int32 ToRemove = Jobs.Num() - MaxHistory;
    int32 Removed = FMath::Min(ToRemove, TerminalJobs.Num());
    for (int32 i = 0; i < Removed; i++)
    {
        Jobs.Remove(TerminalJobs[i].Key);
    }
}

void FCortexGenJobManager::SaveJobs()
{
    // Do not write to disk unless LoadJobs() has been called (guards test isolation).
    if (!bPersistenceEnabled)
    {
        return;
    }

    // Enforce MaxJobHistory before writing to disk
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    const int32 MaxHistory = Settings ? Settings->MaxJobHistory : 50;
    TrimJobHistory(MaxHistory);

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

void FCortexGenJobManager::RecordTiming(const FString& ModelId, float DurationSeconds)
{
    TArray<float>& Samples = TimingData.FindOrAdd(ModelId);
    if (Samples.Num() >= MaxTimingSamples)
    {
        Samples.RemoveAt(0);
    }
    Samples.Add(DurationSeconds);
    SaveTimingData();
}

float FCortexGenJobManager::GetAverageTime(const FString& ModelId) const
{
    const TArray<float>* Samples = TimingData.Find(ModelId);
    if (!Samples || Samples->Num() < MinTimingSamplesForAverage)
    {
        return 0.0f;
    }

    float Sum = 0.0f;
    for (float S : *Samples)
    {
        Sum += S;
    }
    return Sum / Samples->Num();
}

void FCortexGenJobManager::SaveTimingData() const
{
    FString FilePath = FPaths::ProjectSavedDir() / TEXT("CortexGen/timing.json");

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    for (const auto& Pair : TimingData)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> SamplesArray;
        float Sum = 0.0f;
        for (float S : Pair.Value)
        {
            SamplesArray.Add(MakeShared<FJsonValueNumber>(S));
            Sum += S;
        }
        Entry->SetArrayField(TEXT("samples"), SamplesArray);
        Entry->SetNumberField(TEXT("avg"),
            Pair.Value.Num() > 0 ? Sum / Pair.Value.Num() : 0.0f);
        Root->SetObjectField(Pair.Key, Entry);
    }

    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

    // Atomic write-then-rename to prevent corruption on crash
    FString TmpPath = FilePath + TEXT(".tmp");
    if (FFileHelper::SaveStringToFile(JsonString, *TmpPath))
    {
        IFileManager::Get().Move(*FilePath, *TmpPath, true);
    }
}

void FCortexGenJobManager::LoadTimingData()
{
    FString FilePath = FPaths::ProjectSavedDir() / TEXT("CortexGen/timing.json");
    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
    {
        return;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return;
    }

    for (const auto& Pair : Root->Values)
    {
        const TSharedPtr<FJsonObject>* EntryObj;
        if (!Pair.Value->TryGetObject(EntryObj))
        {
            continue;
        }

        const TArray<TSharedPtr<FJsonValue>>* SamplesArray;
        if (!(*EntryObj)->TryGetArrayField(TEXT("samples"), SamplesArray))
        {
            continue;
        }

        TArray<float>& Samples = TimingData.FindOrAdd(Pair.Key);
        for (const auto& Val : *SamplesArray)
        {
            double D;
            if (Val->TryGetNumber(D) && FMath::IsFinite(D) && D > 0.0)
            {
                Samples.Add(static_cast<float>(D));
            }
        }

        // Enforce max
        while (Samples.Num() > MaxTimingSamples)
        {
            Samples.RemoveAt(0);
        }
    }
}
