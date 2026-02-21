#include "CortexCoreCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexAssetOps.h"

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
	};
}
