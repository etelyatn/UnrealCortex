#include "CortexReplayModule.h"

DEFINE_LOG_CATEGORY(LogCortexReplay);

void FCortexReplayModule::StartupModule()
{
	UE_LOG(LogCortexReplay, Log, TEXT("CortexReplay module starting up"));
}

void FCortexReplayModule::ShutdownModule()
{
	UE_LOG(LogCortexReplay, Log, TEXT("CortexReplay module shutting down"));
}

IMPLEMENT_MODULE(FCortexReplayModule, CortexReplay)
