#include "CortexEditorModule.h"
#include "CortexCoreModule.h"
#include "CortexEditorCommandHandler.h"

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
		TEXT("1.0.0"),
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
}

IMPLEMENT_MODULE(FCortexEditorModule, CortexEditor)
