#include "CortexGenModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexGenCommandHandler.h"
#include "CortexGenSettings.h"
#include "Operations/CortexGenJobManager.h"
#include "Providers/CortexGenMeshyProvider.h"
#include "Providers/CortexGenTripoProvider.h"
#include "Providers/CortexGenFalProvider.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY(LogCortexGen);

bool FCortexGenModule::IsEnabled()
{
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    if (!Settings)
    {
        return false;
    }
    return Settings->bEnabled;
}

void FCortexGenModule::StartupModule()
{
    UE_LOG(LogCortexGen, Log, TEXT("CortexGen module starting up"));

    if (!IsEnabled())
    {
        UE_LOG(LogCortexGen, Log, TEXT("CortexGen is disabled in settings. Skipping initialization."));
        return;
    }

    // Save jobs before UObject reflection is torn down.
    // ShutdownModule() runs after UObject teardown, so FJsonObjectConverter would crash there.
    FCoreDelegates::OnEnginePreExit.AddRaw(this, &FCortexGenModule::HandleEnginePreExit);

    // Create job manager (Initialize starts the polling ticker after TSharedFromThis is valid)
    JobManager = MakeShared<FCortexGenJobManager>();
    JobManager->Initialize();
    JobManager->LoadJobs();

    // Register providers based on configured API keys
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    if (Settings)
    {
        if (!Settings->MeshyApiKey.IsEmpty())
        {
            JobManager->RegisterProvider(
                MakeShared<FCortexGenMeshyProvider>(Settings->MeshyApiKey));
        }
        if (!Settings->Tripo3DApiKey.IsEmpty())
        {
            JobManager->RegisterProvider(
                MakeShared<FCortexGenTripoProvider>(Settings->Tripo3DApiKey, Settings->Tripo3DModelVersion));
        }
        if (!Settings->FalApiKey.IsEmpty())
        {
            JobManager->RegisterProvider(
                MakeShared<FCortexGenFalProvider>(
                    Settings->FalApiKey, Settings->FalModelId,
                    Settings->FalImageModelId, Settings->FalQuality));
        }
    }

    // Validate default provider: if the configured default wasn't registered
    // (e.g., only fal key set but DefaultProvider is "meshy"), fall back to
    // the first available provider so jobs don't fail silently.
    if (Settings && !JobManager->GetProvider(Settings->DefaultProvider).IsValid())
    {
        TArray<TSharedPtr<ICortexGenProvider>> Available = JobManager->GetProviders();
        if (Available.Num() > 0)
        {
            UE_LOG(LogCortexGen, Warning,
                TEXT("Default provider '%s' is not registered (missing API key). "
                     "Falling back to '%s'."),
                *Settings->DefaultProvider,
                *Available[0]->GetProviderId().ToString());
            // Note: we don't modify the settings CDO — the fallback is applied
            // at command handler level via the same logic.
        }
    }

    // Register with CortexCore
    ICortexCommandRegistry& Registry =
        FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
        .GetCommandRegistry();

    Registry.RegisterDomain(
        TEXT("gen"),
        TEXT("Cortex Gen"),
        TEXT("1.0.0"),
        MakeShared<FCortexGenCommandHandler>(JobManager)
    );

    UE_LOG(LogCortexGen, Log, TEXT("CortexGen registered with CortexCore"));
}

void FCortexGenModule::ShutdownModule()
{
    UE_LOG(LogCortexGen, Log, TEXT("CortexGen module shutting down"));
    // Safe when disabled: RemoveAll is a no-op if delegate was never bound,
    // and Reset on a null TSharedPtr is a no-op.
    FCoreDelegates::OnEnginePreExit.RemoveAll(this);
    JobManager.Reset();
}

void FCortexGenModule::HandleEnginePreExit()
{
    // Called before UObject reflection is torn down — safe to use FJsonObjectConverter here.
    if (JobManager.IsValid())
    {
        JobManager->SaveJobs();
    }
}

FCortexGenJobManager& FCortexGenModule::GetJobManager() const
{
    checkf(JobManager.IsValid(),
        TEXT("GetJobManager() called but JobManager is null. Is CortexGen disabled in settings?"));
    return *JobManager;
}

IMPLEMENT_MODULE(FCortexGenModule, CortexGen)
