#pragma once

#include "CoreMinimal.h"
#include "CortexGenTypes.h"

class ICortexGenProvider;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnGenJobStateChanged, const FCortexGenJobState&);

/**
 * Manages the lifecycle of AI generation jobs.
 * Non-UObject — owned as TSharedPtr by FCortexGenModule.
 * Persists across PIE sessions (no world dependency).
 *
 * All state mutation must happen on the Game Thread. HTTP callbacks
 * dispatch back via AsyncTask(ENamedThreads::GameThread).
 */
class CORTEXGEN_API FCortexGenJobManager : public TSharedFromThis<FCortexGenJobManager>
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
        FString& OutJobId, FString& OutError, ECortexGenError& OutErrorCode);
    bool CancelJob(const FString& JobId, FString& OutError, ECortexGenError& OutErrorCode);
    bool DeleteJob(const FString& JobId, FString& OutError, ECortexGenError& OutErrorCode);
    bool RetryImport(const FString& JobId, FString& OutError, ECortexGenError& OutErrorCode);

    // Job queries
    const FCortexGenJobState* GetJobState(const FString& JobId) const;
    TArray<FCortexGenJobState> ListJobs(const FString& StatusFilter = TEXT(""),
        int32 Limit = 0) const;

    // Configuration
    void SetMaxConcurrentJobs(int32 Max);

    // Events
    FOnGenJobStateChanged& OnJobStateChanged() { return JobStateChangedDelegate; }

    // Polling (called by ticker on Game Thread)
    void PollActiveJobs();

    /** Must be called after construction to start the polling ticker.
     *  Separate from constructor so TSharedFromThis is valid. */
    void Initialize();

    // Persistence — must be called explicitly (e.g., from StartupModule) to opt in.
    // Test instances that never call LoadJobs() will never write to disk.
    void SaveJobs() const;
    void LoadJobs();

    /** Record a generation timing sample for a model. */
    void RecordTiming(const FString& ModelId, float DurationSeconds);

    /** Get average generation time for a model. Returns 0 if fewer than 3 samples. */
    float GetAverageTime(const FString& ModelId) const;

private:
    static FString GenerateJobId();
    void TransitionJob(FCortexGenJobState& Job, ECortexGenJobStatus NewStatus);
    int32 CountActiveJobs() const;
    static ECortexGenCapability JobTypeToCapability(ECortexGenJobType Type);
    void StartDownloadPipeline(FCortexGenJobState& Job);
    void RunImportPipeline(FCortexGenJobState& Job);
    void BroadcastJobProgress(const FCortexGenJobState& Job);

    void SaveTimingData() const;
    void LoadTimingData();

    TMap<FString, FCortexGenJobState> Jobs;
    TMap<FString, TSharedPtr<ICortexGenProvider>> Providers;  // keyed by provider ID string
    FOnGenJobStateChanged JobStateChangedDelegate;
    int32 MaxConcurrentJobs = 2;
    bool bPersistenceEnabled = false;   // enabled only after LoadJobs() is called

    /** Per-job guard: true while an HTTP poll is in-flight, prevents duplicate polls. */
    TSet<FString> PollsInFlight;

    FTSTicker::FDelegateHandle TickerHandle;

    /** ModelId -> array of timing samples (max 20 per model, sliding window). */
    TMap<FString, TArray<float>> TimingData;
    static constexpr int32 MaxTimingSamples = 20;
    static constexpr int32 MinTimingSamplesForAverage = 3;
};
