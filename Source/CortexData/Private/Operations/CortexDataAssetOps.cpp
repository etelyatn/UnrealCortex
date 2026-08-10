
#include "Operations/CortexDataAssetOps.h"
#include "Operations/CortexDataMutationHelpers.h"
#include "CortexDataModule.h"
#include "CortexLogCapture.h"
#include "CortexSerializer.h"
#include "Engine/DataAsset.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "CortexEditorUtils.h"
#include "ObjectTools.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "Misc/TextBuffer.h"

UDataAsset* FCortexDataAssetOps::LoadDataAsset(const FString& AssetPath, FCortexCommandResult& OutError)
{
	const FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("DataAsset not found: %s"), *AssetPath)
		);
		return nullptr;
	}

	UDataAsset* DataAsset = LoadObject<UDataAsset>(nullptr, *AssetPath);
	if (DataAsset == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("DataAsset not found: %s"), *AssetPath)
		);
	}
	return DataAsset;
}

FCortexCommandResult FCortexDataAssetOps::ListDataAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassFilter;
	FString PathFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("class_filter"), ClassFilter);
		Params->TryGetStringField(TEXT("path_filter"), PathFilter);
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (AssetRegistry == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("AssetRegistry is not available")
		);
	}

	UClass* FilterBaseClass = UDataAsset::StaticClass();
	if (!ClassFilter.IsEmpty())
	{
		UClass* ResolvedClass = FindObject<UClass>(nullptr, *ClassFilter);

		if (!ResolvedClass && !ClassFilter.StartsWith(TEXT("/")))
		{
			const FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassFilter);
			ResolvedClass = FindObject<UClass>(nullptr, *EnginePath);
		}

		if (!ResolvedClass)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Candidate = *It;
				if (!IsValid(Candidate))
				{
					continue;
				}

				if (Candidate->GetName() == ClassFilter || Candidate->GetPathName() == ClassFilter)
				{
					ResolvedClass = Candidate;
					break;
				}
			}
		}

		if (ResolvedClass && ResolvedClass->IsChildOf(UDataAsset::StaticClass()))
		{
			FilterBaseClass = ResolvedClass;
		}
		else
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("data_assets"), TArray<TSharedPtr<FJsonValue>>());
			Data->SetNumberField(TEXT("count"), 0);
			Data->SetField(TEXT("resolved_class"), MakeShared<FJsonValueNull>());
			return FCortexCommandRouter::Success(Data);
		}
	}

	TArray<FAssetData> AssetDataList;

	FARFilter Filter;
	Filter.ClassPaths.Add(FilterBaseClass->GetClassPathName());
	Filter.bRecursiveClasses = true;
	AssetRegistry->GetAssets(Filter, AssetDataList);

	TArray<TSharedPtr<FJsonValue>> ResultArray;

	for (const FAssetData& AssetData : AssetDataList)
	{
		FString AssetPath = AssetData.GetObjectPathString();
		FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();

		if (!PathFilter.IsEmpty() && !AssetPath.StartsWith(PathFilter))
		{
			continue;
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		Entry->SetStringField(TEXT("path"), AssetPath);
		Entry->SetStringField(TEXT("asset_class"), ClassName);
		ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("data_assets"), ResultArray);
	Data->SetNumberField(TEXT("count"), ResultArray.Num());
	if (!ClassFilter.IsEmpty())
	{
		Data->SetStringField(TEXT("resolved_class"), FilterBaseClass->GetName());
	}

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexDataAssetOps::GetDataAsset(const TSharedPtr<FJsonObject>& Params)
{
	auto MakeReflectedReadPolicy = []()
	{
		FCortexSerializationPolicy Policy;
		Policy.Label = ECortexSerializationPolicyLabel::ReflectedRead;
		Policy.bIncludeTextMetadata = true;
		Policy.MaxDepth = 8;
		Policy.bExpandInstancedSubobjects = true;
		return Policy;
	};

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	if (AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Parameter 'asset_path' cannot be empty")
		);
	}

	FCortexLogCapture LogCapture;

	FCortexCommandResult LoadError;
	UDataAsset* DataAsset = LoadDataAsset(AssetPath, LoadError);
	if (DataAsset == nullptr)
	{
		return LoadError;
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	TArray<FString> Warnings = LogCapture.GetWarnings(PackageName);

	UClass* AssetClass = DataAsset->GetClass();

	const FCortexPropertySerializationResult Serialization = FCortexSerializer::ObjectToJsonDeep(DataAsset, MakeReflectedReadPolicy());
	TSharedPtr<FJsonObject> Properties = Serialization.JsonValue.IsValid()
		? Serialization.JsonValue->AsObject()
		: MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("asset_class"), AssetClass->GetName());
	Data->SetObjectField(TEXT("properties"), Properties);
	Data->SetBoolField(TEXT("partial"), Serialization.bPartial);
	Data->SetArrayField(TEXT("issues"), FCortexSerializer::SerializationIssuesToJson(Serialization.Issues));

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		WarningsArray.Reserve(Warnings.Num());
		for (const FString& Warning : Warnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
		}
		Data->SetArrayField(TEXT("warnings"), WarningsArray);
	}

	FCortexCommandResult Result = FCortexCommandRouter::Success(Data);
	Result.Warnings = MoveTemp(Warnings);
	return Result;
}

FCortexCommandResult FCortexDataAssetOps::UpdateDataAsset(const TSharedPtr<FJsonObject>& Params)
{
	check(IsInGameThread());

	FCortexUpdateDataAssetMutationRequest Request;
	FCortexDataMutationResult Result = FCortexDataMutationHelpers::ParseUpdateDataAssetParams(Params, Request);
	if (!Result.bSuccess)
	{
		return Result.ToCommandResult();
	}

	FCortexUpdateDataAssetMutationPlan Plan;
	Result = FCortexDataMutationHelpers::BuildUpdateDataAssetPlan(Request, Plan);
	if (!Result.bSuccess)
	{
		return Result.ToCommandResult();
	}

	Result = Request.bDryRun
		? FCortexDataMutationHelpers::PreviewUpdateDataAsset(Plan)
		: FCortexDataMutationHelpers::ApplyUpdateDataAsset(Plan);
	return Result.ToCommandResult();
}

UClass* FCortexDataAssetOps::ResolveDataAssetClass(const FString& ClassName, FCortexCommandResult& OutError)
{
	// Try full class path first (for example "/Script/Engine.DataAsset").
	UClass* ResolvedClass = FindObject<UClass>(nullptr, *ClassName);

	if (ResolvedClass == nullptr)
	{
		// Check UDataAsset base class by short name.
		if (UDataAsset::StaticClass()->GetName() == ClassName)
		{
			ResolvedClass = UDataAsset::StaticClass();
		}
		else
		{
			// Search all UDataAsset subclasses by short name.
			TArray<UClass*> DerivedClasses;
			GetDerivedClasses(UDataAsset::StaticClass(), DerivedClasses, true);
			for (UClass* Candidate : DerivedClasses)
			{
				if (Candidate != nullptr && Candidate->GetName() == ClassName)
				{
					ResolvedClass = Candidate;
					break;
				}
			}
		}
	}

	if (ResolvedClass == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::ClassNotFound,
			FString::Printf(TEXT("DataAsset class not found: %s"), *ClassName)
		);
		return nullptr;
	}

	if (!ResolvedClass->IsChildOf(UDataAsset::StaticClass()))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Class '%s' is not a UDataAsset subclass"), *ClassName)
		);
		return nullptr;
	}

	// Reject abstract classes - NewObject on abstract triggers ensure in editor.
	if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Cannot instantiate abstract class '%s'. Use a concrete subclass."), *ClassName)
		);
		return nullptr;
	}

	return ResolvedClass;
}

FCortexCommandResult FCortexDataAssetOps::CreateDataAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName;
	FString AssetPath;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("class_name"), ClassName)
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: class_name, asset_path")
		);
	}

	if (ClassName.IsEmpty() || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Parameters 'class_name' and 'asset_path' cannot be empty")
		);
	}

	FCortexCommandResult ClassError;
	UClass* ResolvedClass = ResolveDataAssetClass(ClassName, ClassError);
	if (ResolvedClass == nullptr)
	{
		return ClassError;
	}

	// Normalize object path to package path and derive asset object name.
	const FString PackagePath = FPackageName::ObjectPathToPackageName(AssetPath);
	const FString AssetName = FPackageName::GetShortName(PackagePath);

	if (FindPackage(nullptr, *PackagePath) != nullptr || FPackageName::DoesPackageExist(PackagePath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetAlreadyExists,
			FString::Printf(TEXT("Asset already exists: %s"), *PackagePath)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Create DataAsset %s"), *AssetName)
	));

	UPackage* Package = CreatePackage(*PackagePath);
	UDataAsset* NewAsset = NewObject<UDataAsset>(
		Package,
		ResolvedClass,
		FName(*AssetName),
		RF_Public | RF_Standalone
	);
	if (NewAsset == nullptr)
	{
		Package->MarkAsGarbage();
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to create DataAsset: %s"), *AssetPath)
		);
	}

	TArray<FString> Warnings;
	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropertiesObj)
		&& PropertiesObj != nullptr
		&& (*PropertiesObj).IsValid())
	{
		const bool bApplied = FCortexSerializer::JsonToStruct(*PropertiesObj, ResolvedClass, NewAsset, NewAsset, Warnings);
		if (!bApplied)
		{
			NewAsset->MarkAsGarbage();
			Package->MarkAsGarbage();
			return FCortexCommandRouter::Error(
				CortexErrorCodes::SerializationError,
				TEXT("Failed to apply initial properties to DataAsset")
			);
		}
	}

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewAsset);

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension());

	const FString Directory = FPaths::GetPath(PackageFilename);
	if (!FPaths::DirectoryExists(Directory))
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bSaved = UPackage::SavePackage(Package, NewAsset, *PackageFilename, SaveArgs);
	if (!bSaved)
	{
		NewAsset->MarkAsGarbage();
		Package->MarkAsGarbage();
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to save DataAsset to disk: %s"), *AssetPath)
		);
	}

	FCortexEditorUtils::NotifyAssetModified(NewAsset);

	const FString FullObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), FullObjectPath);
	Data->SetStringField(TEXT("asset_class"), ResolvedClass->GetName());
	Data->SetBoolField(TEXT("created"), true);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FString& Warning : Warnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
		}
		Data->SetArrayField(TEXT("warnings"), WarningsArray);
	}

	UE_LOG(LogCortexData, Log, TEXT("Created DataAsset: %s (class: %s)"), *FullObjectPath, *ClassName);

	FCortexCommandResult Result = FCortexCommandRouter::Success(Data);
	Result.Warnings = MoveTemp(Warnings);
	return Result;
}

FCortexCommandResult FCortexDataAssetOps::DeleteDataAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	if (AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Parameter 'asset_path' cannot be empty")
		);
	}

	// Guard LoadObject against SkipPackage warnings.
	const FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("DataAsset not found: %s"), *AssetPath)
		);
	}

	// Normalize to object path so LoadObject resolves reliably.
	const FString ObjName = FPackageName::GetShortName(PkgName);
	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PkgName, *ObjName);

	UDataAsset* DataAsset = LoadObject<UDataAsset>(nullptr, *ObjectPath);
	if (DataAsset == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("DataAsset not found: %s"), *AssetPath)
		);
	}

	const FString AssetName = DataAsset->GetName();

	// ForceDeleteObjects purges undo buffer, transaction effect is limited.
	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Delete DataAsset %s"), *AssetName)
	));

	UPackage* Package = DataAsset->GetOutermost();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension());

	const auto TryDeletePackageFile = [&PackageFilename](int32 MaxAttempts) -> bool
	{
		for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
		{
			if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
			{
				return true;
			}
			if (IFileManager::Get().Delete(*PackageFilename, false, false, true))
			{
				return true;
			}
			FPlatformProcess::Sleep(0.01f);
		}
		return !FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename);
	};

	// Delete on-disk package first to avoid file watcher race warnings.
	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
	{
		TryDeletePackageFile(20);
	}

	// Guard object prevents false-positive "package marked deleted but modified on disk" warnings
	// if a queued file watcher event races with ForceDeleteObjects.
	UTextBuffer* DeleteGuard = nullptr;
	if (IsValid(Package))
	{
		DeleteGuard = NewObject<UTextBuffer>(Package, TEXT("__CortexDeleteGuard__"), RF_Public);
	}

	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(DataAsset);
	const int32 DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);

	if (DeleteGuard != nullptr)
	{
		DeleteGuard->ClearFlags(RF_Public | RF_Standalone);
	}

	// Retry file deletion after object destruction if file still exists on disk.
	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
	{
		TryDeletePackageFile(20);
	}

	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to delete DataAsset file: %s"), *PackageFilename)
		);
	}

	if (DeletedCount == 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to delete DataAsset: %s (may have references)"), *AssetPath)
		);
	}

	UE_LOG(LogCortexData, Log, TEXT("Deleted DataAsset: %s"), *AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("deleted"), true);

	return FCortexCommandRouter::Success(Data);
}
