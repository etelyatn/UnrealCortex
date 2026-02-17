#include "CortexLevelModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexLevelCommandHandler.h"

DEFINE_LOG_CATEGORY(LogCortexLevel);

void FCortexLevelModule::StartupModule()
{
    UE_LOG(LogCortexLevel, Log, TEXT("CortexLevel module starting up"));

    ICortexCommandRegistry& Registry =
        FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
        .GetCommandRegistry();

    Registry.RegisterDomain(
        TEXT("level"),
        TEXT("Cortex Level"),
        TEXT("1.0.0"),
        MakeShared<FCortexLevelCommandHandler>()
    );

    UE_LOG(LogCortexLevel, Log, TEXT("CortexLevel registered with CortexCore"));
}

void FCortexLevelModule::ShutdownModule()
{
    UE_LOG(LogCortexLevel, Log, TEXT("CortexLevel module shutting down"));
}

IMPLEMENT_MODULE(FCortexLevelModule, CortexLevel)
