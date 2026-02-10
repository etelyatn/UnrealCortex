#include "CortexDataModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogCortexData, Log, All);

void FCortexDataModule::StartupModule()
{
    UE_LOG(LogCortexData, Log, TEXT("CortexData module starting up"));
}

void FCortexDataModule::ShutdownModule()
{
    UE_LOG(LogCortexData, Log, TEXT("CortexData module shutting down"));
}

IMPLEMENT_MODULE(FCortexDataModule, CortexData)
