#include "Operations/CortexLevelLifecycleOps.h"

#include "CortexTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Editor/TemplateMapInfo.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
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

	if (DoesLevelExist(Path))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetAlreadyExists,
			FString::Printf(TEXT("Level already exists at: %s"), *Path));
	}

	FString TemplateName;
	Params->TryGetStringField(TEXT("template"), TemplateName);

	bool bOpen = false;
	Params->TryGetBoolField(TEXT("open"), bOpen);

	if (!GEditor)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("GEditor unavailable"));
	}

	if (bOpen && GEditor->IsPlaySessionInProgress())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorBusy, TEXT("Cannot open level while PIE is active"));
	}

	// Resolve template path if provided
	FString TemplatePath;
	if (!TemplateName.IsEmpty())
	{
		if (TemplateName.StartsWith(TEXT("/")))
		{
			TemplatePath = TemplateName;
		}
		else if (GUnrealEd)
		{
			const TArray<FTemplateMapInfo>& TemplateInfos = GUnrealEd->GetTemplateMapInfos();
			for (const FTemplateMapInfo& Info : TemplateInfos)
			{
				const FString MapPath = Info.Map.GetAssetPathString();
				const FString ShortName = FPackageName::GetShortName(MapPath);
				if (ShortName.Equals(TemplateName, ESearchCase::IgnoreCase) ||
					MapPath.Equals(TemplateName, ESearchCase::IgnoreCase) ||
					Info.DisplayName.ToString().Equals(TemplateName, ESearchCase::IgnoreCase))
				{
					TemplatePath = MapPath;
					break;
				}
			}

			if (TemplatePath.IsEmpty())
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter,
					FString::Printf(TEXT("Template not found: %s. Use list_templates to see available templates."), *TemplateName));
			}
		}
	}

	bool bIsWorldPartition = false;

	if (bOpen)
	{
		if (IsCurrentLevelDirty())
		{
			UEditorLoadingAndSavingUtils::SaveDirtyPackages(true, true);
		}

		UWorld* NewWorld = nullptr;
		if (TemplatePath.IsEmpty())
		{
			NewWorld = UEditorLoadingAndSavingUtils::NewBlankMap(false);
		}
		else
		{
			NewWorld = UEditorLoadingAndSavingUtils::NewMapFromTemplate(TemplatePath, false);
		}

		if (!NewWorld)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Failed to create level"));
		}

		const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(NewWorld, Path);
		if (!bSaved)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::SerializationError,
				FString::Printf(TEXT("Created level but failed to save to: %s"), *Path));
		}

		bIsWorldPartition = NewWorld->IsPartitionedWorld();
	}
	else
	{
		if (!TemplatePath.IsEmpty())
		{
			// Template + open=false: load template, save to dest, restore original
			UWorld* CurrentWorld = GEditor->GetEditorWorldContext().World();
			const FString CurrentLevelPath = CurrentWorld ? CurrentWorld->GetOutermost()->GetName() : TEXT("");

			UWorld* TemplateWorld = UEditorLoadingAndSavingUtils::NewMapFromTemplate(TemplatePath, false);
			if (!TemplateWorld)
			{
				// Restore original level if template load failed
				if (!CurrentLevelPath.IsEmpty())
				{
					const FString CurrentFilePath = FPackageName::LongPackageNameToFilename(CurrentLevelPath, FPackageName::GetMapPackageExtension());
					UEditorLoadingAndSavingUtils::LoadMap(CurrentFilePath);
				}
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Failed to create level from template"));
			}

			bIsWorldPartition = TemplateWorld->IsPartitionedWorld();
			const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(TemplateWorld, Path);

			if (!CurrentLevelPath.IsEmpty())
			{
				const FString CurrentFilePath = FPackageName::LongPackageNameToFilename(CurrentLevelPath, FPackageName::GetMapPackageExtension());
				UEditorLoadingAndSavingUtils::LoadMap(CurrentFilePath);
			}

			if (!bSaved)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::SerializationError,
					FString::Printf(TEXT("Failed to save level to: %s"), *Path));
			}
		}
		else
		{
			// Create blank world without opening it
			const FString PackageName = Path;
			UPackage* Package = CreatePackage(*PackageName);
			if (!Package)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Failed to create package"));
			}

			UWorld* NewWorld = UWorld::CreateWorld(EWorldType::Inactive, false, FName(*FPackageName::GetShortName(Path)), Package);
			if (!NewWorld)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Failed to create world"));
			}

			bIsWorldPartition = NewWorld->IsPartitionedWorld();

			// RF_Standalone must be set on the asset object itself before saving
			NewWorld->SetFlags(RF_Standalone);

			const FString FilePath = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Standalone;
			const bool bSaved = UPackage::SavePackage(Package, NewWorld, *FilePath, SaveArgs);

			NewWorld->DestroyWorld(false);

			if (!bSaved)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::SerializationError,
					FString::Printf(TEXT("Failed to save level to: %s"), *Path));
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), Path);
	Data->SetBoolField(TEXT("world_partition"), bIsWorldPartition);
	return FCortexCommandRouter::Success(Data);
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
