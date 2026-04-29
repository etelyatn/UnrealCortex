#include "CortexStateTreeModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexStateTreeCommandHandler.h"

DEFINE_LOG_CATEGORY(LogCortexStateTree);

void FCortexStateTreeModule::StartupModule()
{
    UE_LOG(LogCortexStateTree, Log, TEXT("CortexStateTree module starting up"));

    ICortexCommandRegistry& Registry =
        FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
        .GetCommandRegistry();

    Registry.RegisterDomain(
        TEXT("statetree"),
        TEXT("Cortex StateTree"),
        TEXT("1.0.0"),
        MakeShared<FCortexStateTreeCommandHandler>());

    UE_LOG(LogCortexStateTree, Log, TEXT("CortexStateTree registered with CortexCore"));
}

void FCortexStateTreeModule::ShutdownModule()
{
    UE_LOG(LogCortexStateTree, Log, TEXT("CortexStateTree module shutting down"));
}

IMPLEMENT_MODULE(FCortexStateTreeModule, CortexStateTree)
