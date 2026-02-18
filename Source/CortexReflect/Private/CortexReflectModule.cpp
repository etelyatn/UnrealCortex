#include "CortexReflectModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexReflectCommandHandler.h"

DEFINE_LOG_CATEGORY(LogCortexReflect);

void FCortexReflectModule::StartupModule()
{
	UE_LOG(LogCortexReflect, Log, TEXT("CortexReflect module starting up"));

	ICortexCommandRegistry& Registry =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
		.GetCommandRegistry();

	Registry.RegisterDomain(
		TEXT("reflect"),
		TEXT("Cortex Reflect"),
		TEXT("1.0.0"),
		MakeShared<FCortexReflectCommandHandler>()
	);

	UE_LOG(LogCortexReflect, Log, TEXT("CortexReflect registered with CortexCore"));
}

void FCortexReflectModule::ShutdownModule()
{
	UE_LOG(LogCortexReflect, Log, TEXT("CortexReflect module shutting down"));
}

IMPLEMENT_MODULE(FCortexReflectModule, CortexReflect)
