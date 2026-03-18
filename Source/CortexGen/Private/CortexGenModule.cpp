#include "CortexGenModule.h"

DEFINE_LOG_CATEGORY(LogCortexGen);

void FCortexGenModule::StartupModule()
{
    UE_LOG(LogCortexGen, Log, TEXT("CortexGen module starting up"));
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
