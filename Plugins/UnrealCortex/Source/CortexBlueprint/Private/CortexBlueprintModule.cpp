#include "CortexBlueprintModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexBPCommandHandler.h"

DEFINE_LOG_CATEGORY(LogCortexBlueprint);

void FCortexBlueprintModule::StartupModule()
{
	UE_LOG(LogCortexBlueprint, Log, TEXT("CortexBlueprint module starting up"));

	ICortexCommandRegistry& Registry =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
		.GetCommandRegistry();

	Registry.RegisterDomain(
		TEXT("bp"),
		TEXT("Cortex Blueprint"),
		TEXT("1.0.0"),
		MakeShared<FCortexBPCommandHandler>()
	);

	UE_LOG(LogCortexBlueprint, Log, TEXT("CortexBlueprint registered with CortexCore"));
}

void FCortexBlueprintModule::ShutdownModule()
{
	UE_LOG(LogCortexBlueprint, Log, TEXT("CortexBlueprint module shutting down"));
}

IMPLEMENT_MODULE(FCortexBlueprintModule, CortexBlueprint)
