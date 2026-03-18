#pragma once

#include "CoreMinimal.h"
#include "CortexGenTypes.h"

class ICortexGenProvider;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnGenJobStateChanged, const FCortexGenJobState&);

/**
 * Manages the lifecycle of AI generation jobs.
 * Non-UObject — owned as TSharedPtr by FCortexGenModule.
 * Persists across PIE sessions (no world dependency).
 */
class FCortexGenJobManager
{
public:
    FCortexGenJobManager();
    ~FCortexGenJobManager();

    // Provider management
    void RegisterProvider(TSharedPtr<ICortexGenProvider> Provider);
    TSharedPtr<ICortexGenProvider> GetProvider(const FString& ProviderId) const;
    TArray<TSharedPtr<ICortexGenProvider>> GetProviders() const;

    // Job operations
    bool SubmitJob(const FString& ProviderId, const FCortexGenJobRequest& Request,
        FString& OutJobId, FString& OutError);
    bool CancelJob(const FString& JobId, FString& OutError);
    bool DeleteJob(const FString& JobId, FString& OutError);
    bool RetryImport(const FString& JobId, FString& OutError);

    // Job queries
    const FCortexGenJobState* GetJobState(const FString& JobId) const;
    TArray<FCortexGenJobState> ListJobs(const FString& StatusFilter = TEXT(""),
        int32 Limit = 0) const;

    // Configuration
    void SetMaxConcurrentJobs(int32 Max);

    // Events
    FOnGenJobStateChanged& OnJobStateChanged() { return JobStateChangedDelegate; }

    // Polling (called by ticker)
    void PollActiveJobs();

    // Persistence — must be called explicitly (e.g., from StartupModule) to opt in.
    // Test instances that never call LoadJobs() will never write to disk.
    void SaveJobs() const;
    void LoadJobs();

private:
    static FString GenerateJobId();
    void TransitionJob(FCortexGenJobState& Job, ECortexGenJobStatus NewStatus);
    int32 CountActiveJobs() const;
    static ECortexGenCapability JobTypeToCapability(ECortexGenJobType Type);
    void StartDownloadPipeline(FCortexGenJobState& Job);
    void RunImportPipeline(FCortexGenJobState& Job);
    void BroadcastJobProgress(const FCortexGenJobState& Job);

    TMap<FString, FCortexGenJobState> Jobs;
    TMap<FString, TSharedPtr<ICortexGenProvider>> Providers;  // keyed by provider ID string
    FOnGenJobStateChanged JobStateChangedDelegate;
    int32 MaxConcurrentJobs = 2;
    bool bPersistenceEnabled = false;   // enabled only after LoadJobs() is called

    FTSTicker::FDelegateHandle TickerHandle;
};
