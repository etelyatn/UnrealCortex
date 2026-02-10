#include "CortexCoreModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogCortex, Log, All);

void FCortexCoreModule::StartupModule()
{
    UE_LOG(LogCortex, Log, TEXT("CortexCore module starting up"));
}

void FCortexCoreModule::ShutdownModule()
{
    UE_LOG(LogCortex, Log, TEXT("CortexCore module shutting down"));
}

IMPLEMENT_MODULE(FCortexCoreModule, CortexCore)
