#include "CortexGenModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexGenCommandHandler.h"
#include "CortexGenSettings.h"
#include "Operations/CortexGenJobManager.h"
#include "Providers/CortexGenMeshyProvider.h"
#include "Providers/CortexGenTripoProvider.h"
#include "Providers/CortexGenFalProvider.h"

DEFINE_LOG_CATEGORY(LogCortexGen);

void FCortexGenModule::StartupModule()
{
    UE_LOG(LogCortexGen, Log, TEXT("CortexGen module starting up"));

    // Create job manager
    JobManager = MakeShared<FCortexGenJobManager>();
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
                MakeShared<FCortexGenTripoProvider>(Settings->Tripo3DApiKey, TEXT("v2.0-20240919")));
        }
        if (!Settings->FalApiKey.IsEmpty())
        {
            JobManager->RegisterProvider(
                MakeShared<FCortexGenFalProvider>(Settings->FalApiKey, Settings->FalModelId));
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
    JobManager.Reset();
}

FCortexGenJobManager& FCortexGenModule::GetJobManager() const
{
    check(JobManager.IsValid());
    return *JobManager;
}

IMPLEMENT_MODULE(FCortexGenModule, CortexGen)
