#include "CortexQAModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexQACommandHandler.h"

DEFINE_LOG_CATEGORY(LogCortexQA);

void FCortexQAModule::StartupModule()
{
    UE_LOG(LogCortexQA, Log, TEXT("CortexQA module starting up"));

    ICortexCommandRegistry& Registry =
        FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
        .GetCommandRegistry();

    Registry.RegisterDomain(
        TEXT("qa"),
        TEXT("Cortex QA"),
        TEXT("1.0.0"),
        MakeShared<FCortexQACommandHandler>());

    UE_LOG(LogCortexQA, Log, TEXT("CortexQA registered with CortexCore"));
}

void FCortexQAModule::ShutdownModule()
{
    UE_LOG(LogCortexQA, Log, TEXT("CortexQA module shutting down"));
}

IMPLEMENT_MODULE(FCortexQAModule, CortexQA)
