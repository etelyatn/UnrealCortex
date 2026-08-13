#include "CortexCoreCommandHandler.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexAssetFingerprintOps.h"
#include "Operations/CortexAssetDeletionOps.h"
#include "Operations/CortexAssetOps.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

namespace
{
const FString CortexPluginVersion = TEXT("0.1.13");

FString BuildPluginBuildId()
{
	return FString::Printf(TEXT("%s-%u"),
		*CortexPluginVersion, FEngineVersion::Current().GetChangelist());
}
}

FCortexCommandResult FCortexCoreCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

	if (Command == TEXT("get_operation_schema"))
	{
		return HandleOperationSchema(Params);
	}
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
	if (Command == TEXT("asset_fingerprint"))
	{
		return FCortexAssetFingerprintOps::AssetFingerprint(Params);
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

FCortexCommandResult FCortexCoreCommandHandler::HandleOperationSchema(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required params: domain, command"));
	}
	FString Domain;
	FString Command;
	if (!Params->TryGetStringField(TEXT("domain"), Domain) || Domain.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: domain"));
	}
	if (!Params->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: command"));
	}

	const TArray<FCortexRegisteredDomain>& Registered = CommandRouter->GetRegisteredDomains();
	const FCortexRegisteredDomain* DomainEntry = nullptr;
	for (const FCortexRegisteredDomain& Entry : Registered)
	{
		if (Entry.Namespace == Domain)
		{
			DomainEntry = &Entry;
			break;
		}
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("domain"), Domain);
	Details->SetStringField(TEXT("command"), Command);

	if (DomainEntry == nullptr)
	{
		const bool bCacheAdvertises = CacheAdvertisesCommand(Domain, Command);
		Details->SetBoolField(TEXT("cache_advertised"), bCacheAdvertises);
		Details->SetBoolField(TEXT("restart_or_reload_required"), bCacheAdvertises);
		Details->SetStringField(TEXT("suggested_action"),
			TEXT("Restart the Unreal Editor with the rebuilt UnrealCortex plugin, or reload the plugin, then re-verify with core.get_operation_schema."));
		return FCortexCommandRouter::Error(CortexErrorCodes::CapabilityCommandNotFound,
			TEXT("Domain not registered by the live editor"), Details);
	}

	const TArray<FCortexCommandInfo> Commands = DomainEntry->Handler->GetSupportedCommands();
	const FCortexCommandInfo* CommandInfo = nullptr;
	for (const FCortexCommandInfo& CmdInfo : Commands)
	{
		if (CmdInfo.Name == Command)
		{
			CommandInfo = &CmdInfo;
			break;
		}
	}

	if (CommandInfo == nullptr)
	{
		const bool bCacheAdvertises = CacheAdvertisesCommand(Domain, Command);
		Details->SetBoolField(TEXT("cache_advertised"), bCacheAdvertises);
		Details->SetBoolField(TEXT("restart_or_reload_required"), bCacheAdvertises);
		Details->SetStringField(TEXT("suggested_action"),
			TEXT("Restart the Unreal Editor with the rebuilt UnrealCortex plugin, or reload the plugin, then re-verify with core.get_operation_schema."));
		return FCortexCommandRouter::Error(CortexErrorCodes::CapabilityCommandNotFound,
			TEXT("Command not registered by the live editor"), Details);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source"), TEXT("live_editor"));
	Data->SetStringField(TEXT("editor_instance_id"),
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore")).GetInstanceId());
	Data->SetStringField(TEXT("plugin_build_id"), BuildPluginBuildId());
	Data->SetStringField(TEXT("domain"), Domain);
	Data->SetStringField(TEXT("command"), Command);
	Data->SetStringField(TEXT("router"), Domain + TEXT("_cmd"));

	TArray<TSharedPtr<FJsonValue>> ParamArray;
	for (const FCortexParamInfo& ParamInfo : CommandInfo->Params)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), ParamInfo.Name);
		ParamObj->SetStringField(TEXT("type"), ParamInfo.Type);
		ParamObj->SetBoolField(TEXT("required"), ParamInfo.bRequired);
		ParamObj->SetStringField(TEXT("description"), ParamInfo.Description);
		ParamArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	Data->SetArrayField(TEXT("params"), ParamArray);
	return FCortexCommandRouter::Success(Data);
}

bool FCortexCoreCommandHandler::CacheAdvertisesCommand(const FString& Domain, const FString& Command) const
{
	const FString CachePath = FPaths::ProjectSavedDir() / TEXT("Cortex/capabilities-cache.json");
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *CachePath))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* DomainsObj = nullptr;
	if (!Root->TryGetObjectField(TEXT("domains"), DomainsObj) || DomainsObj == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* DomainObj = nullptr;
	if (!(*DomainsObj)->TryGetObjectField(Domain, DomainObj) || DomainObj == nullptr)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
	if (!(*DomainObj)->TryGetArrayField(TEXT("commands"), Commands) || Commands == nullptr)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Commands)
	{
		const TSharedPtr<FJsonObject>* Cmd = nullptr;
		if (Value.IsValid() && Value->TryGetObject(Cmd) && Cmd != nullptr)
		{
			FString Name;
			if ((*Cmd)->TryGetStringField(TEXT("name"), Name) && Name == Command)
			{
				return true;
			}
		}
	}
	return false;
}

TArray<FCortexCommandInfo> FCortexCoreCommandHandler::GetSupportedCommands() const
{
	return {
		FCortexCommandInfo{ TEXT("get_operation_schema"), TEXT("Return the live editor's construction contract for one command") }
			.Required(TEXT("domain"), TEXT("string"), TEXT("Domain namespace, e.g. graph"))
			.Required(TEXT("command"), TEXT("string"), TEXT("Command name, e.g. add_node")),
		FCortexCommandInfo{ TEXT("save_asset"), TEXT("Save asset(s) to disk") }
			.OptionalBatchItems(TEXT("Batch items with target, force, dry_run, expected_fingerprint"))
			.OptionalExpectedFingerprint()
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
		FCortexCommandInfo{ TEXT("asset_fingerprint"), TEXT("Read fingerprint metadata for asset path(s)") }
			.Required(TEXT("paths"), TEXT("array"), TEXT("Asset paths to fingerprint")),
		FCortexCommandInfo{ TEXT("delete_asset"), TEXT("Delete a single asset by path") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to delete")),
		FCortexCommandInfo{ TEXT("delete_folder"), TEXT("Delete all assets in a folder") }
			.Required(TEXT("folder_path"), TEXT("string"), TEXT("Folder path to delete"))
			.Optional(TEXT("recursive"), TEXT("boolean"), TEXT("Delete assets in subfolders as well")),
		FCortexCommandInfo{ TEXT("shutdown"), TEXT("Gracefully shut down the editor") }
			.Optional(TEXT("force"), TEXT("boolean"), TEXT("Discard dirty packages before exit")),
		FCortexCommandInfo{ TEXT("batch_query"), TEXT("Alias for batch — execute multiple commands in a single transaction with optional graph rollback") }
			.Required(TEXT("commands"), TEXT("array"), TEXT("Array of command objects (or use 'steps' key)"))
			.Optional(TEXT("steps"), TEXT("array"), TEXT("Alias for commands array"))
			.Optional(TEXT("stop_on_error"), TEXT("boolean"), TEXT("Stop processing on first failure"))
			.Optional(TEXT("rollback_on_error"), TEXT("boolean"), TEXT("Revert nodes/connections created in this batch on failure"))
			.Optional(TEXT("verify_rollback"), TEXT("boolean"), TEXT("Verify rollback removed all created nodes/connections; unverified rollback returns DIRTY_EDITOR_STATE")),
	};
}
