#include "CortexReplayModule.h"

#include "CortexCoreModule.h"
#include "CortexReplayCommandHandler.h"
#include "CortexReplayService.h"
#include "ICortexCommandRegistry.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogCortexReplay);

void FCortexReplayModule::StartupModule()
{
	UE_LOG(LogCortexReplay, Log, TEXT("CortexReplay module starting up"));

	ICortexCommandRegistry& Registry =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
		.GetCommandRegistry();

	// The service is the single native owner of replay orchestration; the transport handler only
	// forwards the five AI operations to it. Human-only operations stay native.
	const TSharedPtr<FCortexReplayService> Service =
		MakeShared<FCortexReplayService>(FPaths::ProjectDir());

	Registry.RegisterDomain(
		TEXT("replay"),
		TEXT("Cortex Replay"),
		TEXT("1.0.0"),
		MakeShared<FCortexReplayCommandHandler>(Service));

	UE_LOG(LogCortexReplay, Log, TEXT("CortexReplay registered with CortexCore"));
}

void FCortexReplayModule::ShutdownModule()
{
	UE_LOG(LogCortexReplay, Log, TEXT("CortexReplay module shutting down"));
}

IMPLEMENT_MODULE(FCortexReplayModule, CortexReplay)
