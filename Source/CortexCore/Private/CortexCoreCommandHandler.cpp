#include "CortexCoreCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexAssetOps.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformMisc.h"
#include "UObject/UObjectIterator.h"

FCortexCommandResult FCortexCoreCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

	if (Command == TEXT("save_asset"))
	{
		return FCortexAssetOps::SaveAsset(Params);
	}
	if (Command == TEXT("open_asset"))
	{
		return FCortexAssetOps::OpenAsset(Params);
	}
	if (Command == TEXT("close_asset"))
	{
		return FCortexAssetOps::CloseAsset(Params);
	}
	if (Command == TEXT("reload_asset"))
	{
		return FCortexAssetOps::ReloadAsset(Params);
	}
	if (Command == TEXT("shutdown"))
	{
		static bool bShutdownRequested = false;
		if (bShutdownRequested)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("Shutdown already in progress"));
		}
		bShutdownRequested = true;

		bool bForce = true;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("force"), bForce);
		}

		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([bForce](float) -> bool
			{
				if (bForce)
				{
					for (TObjectIterator<UPackage> It; It; ++It)
					{
						if (It->IsDirty())
						{
							It->SetDirtyFlag(false);
						}
					}
				}

				FPlatformMisc::RequestExit(false);
				return false;
			}),
			0.1f);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("message"), TEXT("Shutdown initiated"));
		Data->SetBoolField(TEXT("force"), bForce);
		return FCortexCommandRouter::Success(Data);
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown core command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexCoreCommandHandler::GetSupportedCommands() const
{
	return {
		{ TEXT("save_asset"), TEXT("Save asset(s) to disk") },
		{ TEXT("open_asset"), TEXT("Open asset editor tab(s)") },
		{ TEXT("close_asset"), TEXT("Close asset editor tab(s)") },
		{ TEXT("reload_asset"), TEXT("Discard changes and reload asset(s) from disk") },
		{ TEXT("shutdown"), TEXT("Gracefully shut down the editor") },
	};
}
