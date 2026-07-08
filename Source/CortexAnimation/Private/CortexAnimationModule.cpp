#include "CortexAnimationModule.h"
#include "CortexAnimationCommandHandler.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"

DEFINE_LOG_CATEGORY(LogCortexAnimation);

void FCortexAnimationModule::StartupModule()
{
	UE_LOG(LogCortexAnimation, Log, TEXT("CortexAnimation module starting up"));

	ICortexCommandRegistry& Registry =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
		.GetCommandRegistry();

	Registry.RegisterDomain(
		TEXT("anim"),
		TEXT("Cortex Animation"),
		TEXT("1.0.0"),
		MakeShared<FCortexAnimationCommandHandler>()
	);

	UE_LOG(LogCortexAnimation, Log, TEXT("CortexAnimation registered with CortexCore"));
}

void FCortexAnimationModule::ShutdownModule()
{
	UE_LOG(LogCortexAnimation, Log, TEXT("CortexAnimation module shutting down"));
}

IMPLEMENT_MODULE(FCortexAnimationModule, CortexAnimation)
