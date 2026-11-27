#include "CortexEditorModule.h"
#include "CortexCoreModule.h"
#include "CortexEditorCommandHandler.h"
#include "Operations/CortexEditorCursorOps.h"
#include "Operations/CortexEditorStatsOps.h"

DEFINE_LOG_CATEGORY(LogCortexEditor);

void FCortexEditorModule::StartupModule()
{
	UE_LOG(LogCortexEditor, Log, TEXT("CortexEditor module starting up"));

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));

	ICortexCommandRegistry& Registry = CoreModule.GetCommandRegistry();

	auto Handler = MakeShared<FCortexEditorCommandHandler>();
	Registry.RegisterDomain(
		TEXT("editor"),
		TEXT("Cortex Editor"),
		TEXT("1.0.1"),
		Handler);

	CoreModule.SetClientDisconnectCallback([Handler]()
	{
		Handler->OnTcpClientDisconnected();
	});

	UE_LOG(LogCortexEditor, Log, TEXT("CortexEditor registered with CortexCore"));
}

void FCortexEditorModule::ShutdownModule()
{
	UE_LOG(LogCortexEditor, Log, TEXT("CortexEditor module shutting down"));
	FCortexEditorStatsOps::Shutdown();
	FCortexEditorCursorOps::Shutdown();
}

IMPLEMENT_MODULE(FCortexEditorModule, CortexEditor)
