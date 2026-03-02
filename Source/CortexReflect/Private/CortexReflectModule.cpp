#include "CortexReflectModule.h"
#include "CortexCoreModule.h"
#include "ICortexCommandRegistry.h"
#include "CortexReflectCommandHandler.h"
#include "Engine/Engine.h"
#include "Operations/CortexReflectOps.h"

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

	if (GEngine && GEngine->IsInitialized())
	{
		OnPostEngineInit();
	}
	else
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(
			this,
			&FCortexReflectModule::OnPostEngineInit
		);
	}

	UE_LOG(LogCortexReflect, Log, TEXT("CortexReflect registered with CortexCore"));
}

void FCortexReflectModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	UE_LOG(LogCortexReflect, Log, TEXT("CortexReflect module shutting down"));
}

void FCortexReflectModule::OnPostEngineInit()
{
	RunAutoScan();
}

void FCortexReflectModule::RunAutoScan()
{
	UE_LOG(LogCortexReflect, Log, TEXT("Auto-scanning project class hierarchy..."));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("root"), TEXT("AActor"));
	Params->SetNumberField(TEXT("depth"), 10);
	Params->SetNumberField(TEXT("max_results"), 5000);
	Params->SetBoolField(TEXT("include_engine"), false);

	const FCortexCommandResult Result = FCortexReflectOps::ClassHierarchy(Params);
	if (Result.bSuccess)
	{
		int32 TotalClasses = 0;
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetNumberField(TEXT("total_classes"), TotalClasses);
		}
		UE_LOG(LogCortexReflect, Log, TEXT("Auto-scan complete: %d classes cached"), TotalClasses);
	}
	else
	{
		UE_LOG(LogCortexReflect, Warning, TEXT("Auto-scan failed: %s"), *Result.ErrorMessage);
	}
}

IMPLEMENT_MODULE(FCortexReflectModule, CortexReflect)
