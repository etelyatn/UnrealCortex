#include "CortexGraphModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexGraphCommandHandler.h"

DEFINE_LOG_CATEGORY(LogCortexGraph);

void FCortexGraphModule::StartupModule()
{
	UE_LOG(LogCortexGraph, Log, TEXT("CortexGraph module starting up"));

	ICortexCommandRegistry& Registry =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
		.GetCommandRegistry();

	Registry.RegisterDomain(
		TEXT("graph"),
		TEXT("Cortex Graph"),
		TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>()
	);

	UE_LOG(LogCortexGraph, Log, TEXT("CortexGraph registered with CortexCore"));
}

void FCortexGraphModule::ShutdownModule()
{
	UE_LOG(LogCortexGraph, Log, TEXT("CortexGraph module shutting down"));
}

IMPLEMENT_MODULE(FCortexGraphModule, CortexGraph)
