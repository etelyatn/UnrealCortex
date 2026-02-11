#include "CortexUMGModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexUMGCommandHandler.h"

DEFINE_LOG_CATEGORY(LogCortexUMG);

void FCortexUMGModule::StartupModule()
{
    UE_LOG(LogCortexUMG, Log, TEXT("CortexUMG module starting up"));

    ICortexCommandRegistry& Registry =
        FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
        .GetCommandRegistry();

    Registry.RegisterDomain(
        TEXT("umg"),
        TEXT("Cortex UMG"),
        TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>()
    );

    UE_LOG(LogCortexUMG, Log, TEXT("CortexUMG registered with CortexCore"));
}

void FCortexUMGModule::ShutdownModule()
{
    UE_LOG(LogCortexUMG, Log, TEXT("CortexUMG module shutting down"));
}

IMPLEMENT_MODULE(FCortexUMGModule, CortexUMG)
