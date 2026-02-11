#include "CortexBlueprintModule.h"

DEFINE_LOG_CATEGORY(LogCortexBlueprint);

void FCortexBlueprintModule::StartupModule()
{
	UE_LOG(LogCortexBlueprint, Log, TEXT("CortexBlueprint module starting up"));
}

void FCortexBlueprintModule::ShutdownModule()
{
	UE_LOG(LogCortexBlueprint, Log, TEXT("CortexBlueprint module shutting down"));
}

IMPLEMENT_MODULE(FCortexBlueprintModule, CortexBlueprint)
