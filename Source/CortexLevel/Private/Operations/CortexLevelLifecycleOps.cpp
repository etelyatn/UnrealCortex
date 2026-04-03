#include "Operations/CortexLevelLifecycleOps.h"

#include "CortexTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Editor/TemplateMapInfo.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/LevelStreaming.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "UnrealEdGlobals.h"

FCortexCommandResult FCortexLevelLifecycleOps::ListTemplates(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> TemplateArray;

	// Always include the built-in empty level option first
	{
		TSharedPtr<FJsonObject> EmptyTemplate = MakeShared<FJsonObject>();
		EmptyTemplate->SetStringField(TEXT("name"), TEXT("Empty Level"));
		EmptyTemplate->SetStringField(TEXT("path"), TEXT(""));
		TemplateArray.Add(MakeShared<FJsonValueObject>(EmptyTemplate));
	}

	if (GUnrealEd)
	{
		const TArray<FTemplateMapInfo>& TemplateMapInfos = GUnrealEd->GetTemplateMapInfos();
		for (const FTemplateMapInfo& Info : TemplateMapInfos)
		{
			TSharedPtr<FJsonObject> TemplateJson = MakeShared<FJsonObject>();

			const FString MapPath = Info.Map.GetAssetPathString();
			const FString DisplayName = Info.DisplayName.IsEmpty()
				? FPackageName::GetShortName(MapPath)
				: Info.DisplayName.ToString();

			TemplateJson->SetStringField(TEXT("name"), DisplayName);
			TemplateJson->SetStringField(TEXT("path"), MapPath);

			if (!Info.Category.IsEmpty())
			{
				TemplateJson->SetStringField(TEXT("category"), Info.Category);
			}

			TemplateArray.Add(MakeShared<FJsonValueObject>(TemplateJson));
		}
	}
	else
	{
		// Fallback: scan engine templates via AssetRegistry
		const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		FARFilter Filter;
		Filter.PackagePaths.Add(TEXT("/Engine/Maps/Templates"));
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);

		for (const FAssetData& Asset : Assets)
		{
			TSharedPtr<FJsonObject> TemplateJson = MakeShared<FJsonObject>();
			TemplateJson->SetStringField(TEXT("name"), Asset.AssetName.ToString());
			TemplateJson->SetStringField(TEXT("path"), Asset.PackageName.ToString());
			TemplateArray.Add(MakeShared<FJsonValueObject>(TemplateJson));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("templates"), TemplateArray);
	Data->SetNumberField(TEXT("count"), TemplateArray.Num());
	return FCortexCommandRouter::Success(Data);
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
