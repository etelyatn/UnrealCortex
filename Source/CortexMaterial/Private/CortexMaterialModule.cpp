#include "CortexMaterialModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexMaterialCommandHandler.h"

DEFINE_LOG_CATEGORY(LogCortexMaterial);

void FCortexMaterialModule::StartupModule()
{
	UE_LOG(LogCortexMaterial, Log, TEXT("CortexMaterial module starting up"));

	ICortexCommandRegistry& Registry =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
		.GetCommandRegistry();

	Registry.RegisterDomain(
		TEXT("material"),
		TEXT("Cortex Material"),
		TEXT("1.0.0"),
		MakeShared<FCortexMaterialCommandHandler>()
	);

	UE_LOG(LogCortexMaterial, Log, TEXT("CortexMaterial registered with CortexCore"));
}

void FCortexMaterialModule::ShutdownModule()
{
	UE_LOG(LogCortexMaterial, Log, TEXT("CortexMaterial module shutting down"));
}

IMPLEMENT_MODULE(FCortexMaterialModule, CortexMaterial)
