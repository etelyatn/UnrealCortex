#include "CortexDataModule.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"

DEFINE_LOG_CATEGORY_STATIC(LogCortexData, Log, All);

void FCortexDataModule::StartupModule()
{
    UE_LOG(LogCortexData, Log, TEXT("CortexData module starting up"));

    FCortexCoreModule& CoreModule =
        FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));

    CoreModule.GetCommandRouter().SetDefaultHandler(
        [](const FString& Command, const TSharedPtr<FJsonObject>& Params)
        {
            return FCortexDataCommandHandler::Execute(Command, Params);
        }
    );

    UE_LOG(LogCortexData, Log, TEXT("CortexData registered %d commands with CortexCore"), 29);
}

void FCortexDataModule::ShutdownModule()
{
    UE_LOG(LogCortexData, Log, TEXT("CortexData module shutting down"));
}

IMPLEMENT_MODULE(FCortexDataModule, CortexData)
