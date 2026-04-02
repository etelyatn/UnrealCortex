#include "Operations/CortexLevelLifecycleOps.h"

#include "CortexTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/LevelStreaming.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"

FCortexCommandResult FCortexLevelLifecycleOps::ListTemplates(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

FCortexCommandResult FCortexLevelLifecycleOps::CreateLevel(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("Missing params"));
	}

	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("Missing required parameter: path"));
	}

	if (!IsValidContentPath(Path))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter,
			FString::Printf(TEXT("Invalid content path: %s. Must start with /Game/ or /Plugins/"), *Path));
	}

	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

FCortexCommandResult FCortexLevelLifecycleOps::OpenLevel(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

FCortexCommandResult FCortexLevelLifecycleOps::DuplicateLevel(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

FCortexCommandResult FCortexLevelLifecycleOps::RenameLevel(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

FCortexCommandResult FCortexLevelLifecycleOps::DeleteLevel(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

bool FCortexLevelLifecycleOps::IsLevelCurrentlyOpen(const FString& ContentPath)
{
	if (!GEditor)
	{
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return false;
	}

	const FString CurrentPath = World->GetOutermost()->GetName();
	if (CurrentPath == ContentPath)
	{
		return true;
	}

	for (ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		if (Streaming && Streaming->GetWorldAssetPackageName() == ContentPath)
		{
			return true;
		}
	}

	return false;
}

bool FCortexLevelLifecycleOps::IsCurrentLevelDirty()
{
	if (!GEditor)
	{
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return false;
	}

	if (World->GetOutermost()->IsDirty())
	{
		return true;
	}

	for (ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		if (Streaming && Streaming->IsLevelLoaded())
		{
			if (ULevel* LoadedLevel = Streaming->GetLoadedLevel())
			{
				if (LoadedLevel->GetOutermost()->IsDirty())
				{
					return true;
				}
			}
		}
	}

	return false;
}

bool FCortexLevelLifecycleOps::IsValidContentPath(const FString& Path)
{
	return Path.StartsWith(TEXT("/Game/")) || Path.StartsWith(TEXT("/Plugins/"));
}

bool FCortexLevelLifecycleOps::DoesLevelExist(const FString& ContentPath)
{
	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	const FString ObjectPath = ContentPath + TEXT(".") + FPackageName::GetShortName(ContentPath);
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	if (AssetData.IsValid())
	{
		return true;
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(ContentPath);
	return FPackageName::DoesPackageExist(PackageName);
}
