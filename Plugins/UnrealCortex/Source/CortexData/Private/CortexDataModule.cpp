#include "CortexDataModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexDataCommandHandler.h"

DEFINE_LOG_CATEGORY_STATIC(LogCortexData, Log, All);

void FCortexDataModule::StartupModule()
{
    UE_LOG(LogCortexData, Log, TEXT("CortexData module starting up"));

    ICortexCommandRegistry& Registry =
        FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
        .GetCommandRegistry();

    Registry.RegisterDomain(
        TEXT("data"),
        TEXT("Cortex Data"),
        TEXT("1.0.0"),
        MakeShared<FCortexDataCommandHandler>()
    );

    UE_LOG(LogCortexData, Log, TEXT("CortexData registered with CortexCore"));
}

void FCortexDataModule::ShutdownModule()
{
    UE_LOG(LogCortexData, Log, TEXT("CortexData module shutting down"));
}

IMPLEMENT_MODULE(FCortexDataModule, CortexData)
