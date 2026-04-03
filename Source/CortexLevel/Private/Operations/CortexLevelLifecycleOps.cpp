#include "Operations/CortexLevelLifecycleOps.h"

#include "CortexTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Editor/TemplateMapInfo.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "ISourceControlModule.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Subsystems/EditorAssetSubsystem.h"
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
			// Template + open=false: load template package without world transition, then duplicate and save.
			// This keeps GEditor->GetEditorWorldContext().World() unchanged throughout.
			UPackage* TemplatePackage = LoadPackage(nullptr, *TemplatePath, LOAD_None);
			if (!TemplatePackage)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("Failed to load template: %s"), *TemplatePath));
			}

			UWorld* TemplateWorld = FindObjectFast<UWorld>(TemplatePackage,
				FName(*FPackageName::GetShortName(TemplatePath)));
			if (!TemplateWorld)
			{
				// Fallback: scan the package for any UWorld
				ForEachObjectWithPackage(TemplatePackage, [&TemplateWorld](UObject* Obj)
				{
					if (!TemplateWorld && Obj && Obj->IsA<UWorld>())
					{
						TemplateWorld = Cast<UWorld>(Obj);
					}
					return true;
				}, false);
			}

			if (!TemplateWorld)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					TEXT("Template package does not contain a world"));
			}

			// Duplicate the template world into the destination package
			UPackage* DestPackage = CreatePackage(*Path);
			FObjectDuplicationParameters DupParams(TemplateWorld, DestPackage);
			DupParams.DestName = FName(*FPackageName::GetShortName(Path));
			DupParams.DuplicateMode = EDuplicateMode::Normal;
			UWorld* NewWorld = Cast<UWorld>(StaticDuplicateObjectEx(DupParams));
			if (!NewWorld)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					TEXT("Failed to duplicate template world"));
			}

			NewWorld->SetFlags(RF_Standalone);
			bIsWorldPartition = NewWorld->IsPartitionedWorld();

			const FString FilePath = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetMapPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Standalone;
			const bool bSaved = UPackage::SavePackage(DestPackage, NewWorld, *FilePath, SaveArgs);

			NewWorld->DestroyWorld(false);

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
			FString::Printf(TEXT("Invalid content path: %s"), *Path));
	}

	if (!DoesLevelExist(Path))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Level not found: %s"), *Path));
	}

	if (!GEditor)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("GEditor unavailable"));
	}

	if (GEditor->IsPlaySessionInProgress())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorBusy, TEXT("Cannot open level while PIE is active"));
	}

	bool bSaveCurrent = false;
	Params->TryGetBoolField(TEXT("save_current"), bSaveCurrent);

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	if (!bSaveCurrent && !bForce && IsCurrentLevelDirty())
	{
		TSharedPtr<FJsonObject> ErrorDetails = MakeShared<FJsonObject>();
		ErrorDetails->SetBoolField(TEXT("is_dirty"), true);
		return FCortexCommandRouter::Error(CortexErrorCodes::UnsavedChanges,
			TEXT("Current level has unsaved changes. Use save_current=true to save first, or force=true to discard."),
			ErrorDetails);
	}

	if (bSaveCurrent)
	{
		FEditorFileUtils::SaveDirtyPackages(false, true, true, true);
	}

	const FString FilePath = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetMapPackageExtension());
	UWorld* NewWorld = UEditorLoadingAndSavingUtils::LoadMap(FilePath);

	if (!NewWorld)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Failed to open level: %s"), *Path));
	}

	int32 ActorCount = 0;
	for (TActorIterator<AActor> It(NewWorld); It; ++It)
	{
		if (IsValid(*It))
		{
			++ActorCount;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), NewWorld->GetMapName());
	Data->SetStringField(TEXT("path"), NewWorld->GetOutermost()->GetName());
	Data->SetNumberField(TEXT("actor_count"), ActorCount);
	Data->SetBoolField(TEXT("world_partition"), NewWorld->GetWorldPartition() != nullptr);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelLifecycleOps::DuplicateLevel(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("Missing params"));
	}

	FString SourcePath;
	if (!Params->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("Missing required parameter: source_path"));
	}

	FString DestPath;
	if (!Params->TryGetStringField(TEXT("dest_path"), DestPath) || DestPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("Missing required parameter: dest_path"));
	}

	if (!IsValidContentPath(SourcePath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter,
			FString::Printf(TEXT("Invalid source path: %s"), *SourcePath));
	}

	if (!IsValidContentPath(DestPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter,
			FString::Printf(TEXT("Invalid dest path: %s"), *DestPath));
	}

	if (!DoesLevelExist(SourcePath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Source level not found: %s"), *SourcePath));
	}

	if (DoesLevelExist(DestPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetAlreadyExists,
			FString::Printf(TEXT("Destination already exists: %s"), *DestPath));
	}

	if (!GEditor)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("GEditor unavailable"));
	}

	if (GEditor->IsPlaySessionInProgress())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorBusy, TEXT("Cannot duplicate level while PIE is active"));
	}

	// Use template+save approach — UEditorAssetSubsystem::DuplicateAsset does not handle World assets reliably
	UWorld* CurrentWorld = GEditor->GetEditorWorldContext().World();
	const FString CurrentLevelPath = CurrentWorld ? CurrentWorld->GetOutermost()->GetName() : TEXT("");

	const FString SourceFile = FPackageName::LongPackageNameToFilename(SourcePath, FPackageName::GetMapPackageExtension());
	UWorld* TemplateWorld = UEditorLoadingAndSavingUtils::NewMapFromTemplate(SourceFile, false);
	if (!TemplateWorld)
	{
		if (!CurrentLevelPath.IsEmpty())
		{
			const FString CurrentFile = FPackageName::LongPackageNameToFilename(CurrentLevelPath, FPackageName::GetMapPackageExtension());
			UEditorLoadingAndSavingUtils::LoadMap(CurrentFile);
		}
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Failed to duplicate level: %s"), *SourcePath));
	}

	const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(TemplateWorld, DestPath);

	// Restore original level
	if (!CurrentLevelPath.IsEmpty())
	{
		const FString CurrentFile = FPackageName::LongPackageNameToFilename(CurrentLevelPath, FPackageName::GetMapPackageExtension());
		UEditorLoadingAndSavingUtils::LoadMap(CurrentFile);
	}

	if (!bSaved)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to save duplicated level to: %s"), *DestPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), DestPath);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelLifecycleOps::RenameLevel(const TSharedPtr<FJsonObject>& Params)
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

	FString NewPath;
	if (!Params->TryGetStringField(TEXT("new_path"), NewPath) || NewPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("Missing required parameter: new_path"));
	}

	if (!IsValidContentPath(Path) || !IsValidContentPath(NewPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("Invalid content path"));
	}

	if (!DoesLevelExist(Path))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Level not found: %s"), *Path));
	}

	if (DoesLevelExist(NewPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetAlreadyExists,
			FString::Printf(TEXT("Destination already exists: %s"), *NewPath));
	}

	if (IsLevelCurrentlyOpen(Path))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::LevelInUse,
			FString::Printf(TEXT("Cannot rename currently open or loaded level: %s"), *Path));
	}

	if (!GEditor)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("GEditor unavailable"));
	}

	UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
	if (!AssetSubsystem)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("EditorAssetSubsystem unavailable"));
	}

	const bool bRenamed = AssetSubsystem->RenameAsset(Path, NewPath);
	if (!bRenamed)
	{
		ISourceControlModule& SCCModule = ISourceControlModule::Get();
		if (SCCModule.IsEnabled())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::SourceControlError,
				FString::Printf(TEXT("Failed to rename level. Source control may be blocking: %s"), *Path));
		}

		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Failed to rename level: %s"), *Path));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("old_path"), Path);
	Data->SetStringField(TEXT("new_path"), NewPath);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelLifecycleOps::DeleteLevel(const TSharedPtr<FJsonObject>& Params)
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
			FString::Printf(TEXT("Invalid content path: %s"), *Path));
	}

	if (!DoesLevelExist(Path))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Level not found: %s"), *Path));
	}

	if (IsLevelCurrentlyOpen(Path))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::LevelInUse,
			FString::Printf(TEXT("Cannot delete currently open or loaded level: %s"), *Path));
	}

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	if (!GEditor)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("GEditor unavailable"));
	}

	// Check referencers unless force=true
	if (!bForce)
	{
		const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetIdentifier> Referencers;
		AssetRegistry.GetReferencers(FAssetIdentifier(FName(*Path)), Referencers);

		if (Referencers.Num() > 0)
		{
			TSharedPtr<FJsonObject> RefDetails = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> ReferencerArray;
			for (const FAssetIdentifier& Ref : Referencers)
			{
				ReferencerArray.Add(MakeShared<FJsonValueString>(Ref.PackageName.ToString()));
			}
			RefDetails->SetArrayField(TEXT("referencers"), ReferencerArray);
			RefDetails->SetNumberField(TEXT("count"), Referencers.Num());

			return FCortexCommandRouter::Error(CortexErrorCodes::HasReferences,
				FString::Printf(TEXT("Level has %d referencers. Use force=true to delete anyway."), Referencers.Num()),
				RefDetails);
		}
	}

	// Guard LoadPackage with existence check to prevent SkipPackage warnings
	const FString PkgName = FPackageName::ObjectPathToPackageName(Path);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Package not found on disk: %s"), *Path));
	}

	UPackage* Package = FindPackage(nullptr, *PkgName);
	if (!Package)
	{
		Package = LoadPackage(nullptr, *PkgName, LOAD_None);
	}

	if (!Package)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Failed to load package for deletion: %s"), *Path));
	}

	UObject* LevelAsset = FindObjectFast<UWorld>(Package, FName(*FPackageName::GetShortName(Path)));
	if (!LevelAsset)
	{
		// Try finding any UWorld in the package
		ForEachObjectWithPackage(Package, [&LevelAsset](UObject* Obj)
		{
			if (!LevelAsset && Obj && Obj->IsA<UWorld>())
			{
				LevelAsset = Obj;
			}
			return true;
		}, false);
	}

	if (!LevelAsset)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Failed to find UWorld object in package: %s"), *Path));
	}

	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(LevelAsset);
	const int32 DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);

	if (DeletedCount == 0)
	{
		ISourceControlModule& SCCModule = ISourceControlModule::Get();
		if (SCCModule.IsEnabled())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::SourceControlError,
				FString::Printf(TEXT("Failed to delete level. Source control may be blocking: %s"), *Path));
		}

		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Failed to delete level: %s"), *Path));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("deleted_path"), Path);
	return FCortexCommandRouter::Success(Data);
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
