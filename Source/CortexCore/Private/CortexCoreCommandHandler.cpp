#include "CortexCoreCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexAssetDeletionOps.h"
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
	if (Command == TEXT("delete_asset"))
	{
		return FCortexAssetDeletionOps::DeleteAsset(Params);
	}
	if (Command == TEXT("delete_folder"))
	{
		return FCortexAssetDeletionOps::DeleteFolder(Params);
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
		FCortexCommandInfo{ TEXT("save_asset"), TEXT("Save asset(s) to disk") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path, paths, or glob to save"))
			.Optional(TEXT("force"), TEXT("boolean"), TEXT("Save even when the asset is not dirty"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview which assets would be saved")),
		FCortexCommandInfo{ TEXT("open_asset"), TEXT("Open asset editor tab(s)") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path, paths, or glob to open"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview which assets would be opened")),
		FCortexCommandInfo{ TEXT("close_asset"), TEXT("Close asset editor tab(s)") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path, paths, or glob to close"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save dirty assets before closing"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview which assets would be closed")),
		FCortexCommandInfo{ TEXT("reload_asset"), TEXT("Discard changes and reload asset(s) from disk") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path, paths, or glob to reload"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview which assets would be reloaded")),
		FCortexCommandInfo{ TEXT("delete_asset"), TEXT("Delete a single asset by path") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to delete")),
		FCortexCommandInfo{ TEXT("delete_folder"), TEXT("Delete all assets in a folder") }
			.Required(TEXT("folder_path"), TEXT("string"), TEXT("Folder path to delete"))
			.Optional(TEXT("recursive"), TEXT("boolean"), TEXT("Delete assets in subfolders as well")),
		FCortexCommandInfo{ TEXT("shutdown"), TEXT("Gracefully shut down the editor") }
			.Optional(TEXT("force"), TEXT("boolean"), TEXT("Discard dirty packages before exit")),
		FCortexCommandInfo{ TEXT("batch_query"), TEXT("Alias for batch — execute multiple commands in a single transaction") }
			.Required(TEXT("commands"), TEXT("array"), TEXT("Array of command objects (or use 'steps' key)"))
			.Optional(TEXT("steps"), TEXT("array"), TEXT("Alias for commands array"))
			.Optional(TEXT("stop_on_error"), TEXT("boolean"), TEXT("Stop processing on first failure")),
	};
}
