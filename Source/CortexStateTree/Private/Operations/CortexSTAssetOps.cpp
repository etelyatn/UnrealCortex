#include "Operations/CortexSTAssetOps.h"

#include "CortexCommandRouter.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeModule.h"
#include "CortexEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "StateTreeFactory.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/TextBuffer.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "UObject/SavePackage.h"

namespace
{
TArray<TSharedPtr<FJsonValue>> MakeReferencerArray(const TArray<FAssetIdentifier>& Referencers)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Referencers.Num());
	for (const FAssetIdentifier& Referencer : Referencers)
	{
		Values.Add(MakeShared<FJsonValueString>(Referencer.ToString()));
	}
	return Values;
}

bool TryDeletePackageFile(const FString& PackageFilename, const int32 MaxAttempts = 20)
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
}

UClass* ResolveSchemaClass(const FString& SchemaClassPath)
{
	if (SchemaClassPath.IsEmpty())
	{
		return nullptr;
	}

	if (UClass* FoundClass = FindObject<UClass>(nullptr, *SchemaClassPath))
	{
		return FoundClass;
	}

	if (UClass* LooseMatch = FindFirstObject<UClass>(*SchemaClassPath, EFindFirstObjectOptions::NativeFirst))
	{
		return LooseMatch;
	}

	const FString ObjectPath = FPackageName::ExportTextPathToObjectPath(SchemaClassPath);
	if (!ObjectPath.Contains(TEXT(".")))
	{
		return nullptr;
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
	if (PackageName.IsEmpty()
		|| !FPackageName::IsValidLongPackageName(PackageName, true)
		|| (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName)))
	{
		return nullptr;
	}

	return LoadObject<UClass>(nullptr, *ObjectPath);
}

FCortexCommandResult MakeInvalidSchemaError(
	const FString& SchemaClassPath,
	const FString& Message,
	const TArray<FString>& RejectedFlags = {})
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("schema_class"), SchemaClassPath);
	if (RejectedFlags.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> RejectedFlagValues;
		RejectedFlagValues.Reserve(RejectedFlags.Num());
		for (const FString& RejectedFlag : RejectedFlags)
		{
			RejectedFlagValues.Add(MakeShared<FJsonValueString>(RejectedFlag));
		}
		Details->SetArrayField(TEXT("rejected_flags"), RejectedFlagValues);
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::StateTreeSchemaInvalid,
		Message,
		Details);
}

FCortexCommandResult ValidateSchemaClassForCreation(UClass* SchemaClass, const FString& SchemaClassPath)
{
	if (!SchemaClass)
	{
		return MakeInvalidSchemaError(
			SchemaClassPath,
			FString::Printf(TEXT("StateTree schema class not found: %s"), *SchemaClassPath));
	}

	if (!SchemaClass->IsChildOf(UStateTreeSchema::StaticClass()))
	{
		return MakeInvalidSchemaError(
			SchemaClassPath,
			FString::Printf(TEXT("Class is not a StateTree schema: %s"), *SchemaClassPath));
	}

	TArray<FString> RejectedFlags;
	if (SchemaClass->HasAnyClassFlags(CLASS_Abstract))
	{
		RejectedFlags.Add(TEXT("Abstract"));
	}
	if (SchemaClass->HasAnyClassFlags(CLASS_Deprecated))
	{
		RejectedFlags.Add(TEXT("Deprecated"));
	}
	if (SchemaClass->HasAnyClassFlags(CLASS_NewerVersionExists))
	{
		RejectedFlags.Add(TEXT("NewerVersionExists"));
	}
	if (SchemaClass->HasAnyClassFlags(CLASS_HideDropDown))
	{
		RejectedFlags.Add(TEXT("HideDropdown"));
	}

	if (RejectedFlags.Num() > 0)
	{
		return MakeInvalidSchemaError(
			SchemaClassPath,
			FString::Printf(TEXT("StateTree schema class is not selectable for asset creation: %s"), *SchemaClassPath),
			RejectedFlags);
	}

	return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
}

TSharedPtr<FJsonObject> MakeListEntry(const FAssetData& AssetData)
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
	Entry->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
	Entry->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
	return Entry;
}
}

FCortexCommandResult FCortexSTAssetOps::ListAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter = TEXT("/Game");
	int32 Limit = 1000;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("path_filter"), PathFilter);
		Params->TryGetNumberField(TEXT("limit"), Limit);
	}

	PathFilter = FCortexEditorUtils::NormalizeMountedContentPath(PathFilter);

	Limit = FMath::Max(Limit, 0);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UStateTree::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
		Filter.bRecursivePaths = true;
	}

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString() < B.GetObjectPathString();
	});

	TArray<TSharedPtr<FJsonValue>> AssetValues;
	const int32 ResultCount = Limit > 0 ? FMath::Min(Limit, Assets.Num()) : Assets.Num();
	AssetValues.Reserve(ResultCount);
	for (int32 Index = 0; Index < ResultCount; ++Index)
	{
		AssetValues.Add(MakeShared<FJsonValueObject>(MakeListEntry(Assets[Index])));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("assets"), AssetValues);
	Data->SetNumberField(TEXT("count"), AssetValues.Num());
	Data->SetStringField(TEXT("path_filter"), PathFilter);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexSTAssetOps::CreateAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	FString PackageName;
	if (!CortexST::ValidateWritablePackage(AssetPath, PackageName, Error))
	{
		return Error;
	}

	if (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeAlreadyExists,
			FString::Printf(TEXT("StateTree already exists: %s"), *AssetPath));
	}

	FString SchemaClassPath;
	if (!CortexST::GetRequiredString(Params, TEXT("schema_class"), SchemaClassPath, Error))
	{
		return Error;
	}

	UClass* SchemaClass = ResolveSchemaClass(SchemaClassPath);
	const FCortexCommandResult SchemaValidation = ValidateSchemaClassForCreation(SchemaClass, SchemaClassPath);
	if (!SchemaValidation.bSuccess)
	{
		return SchemaValidation;
	}

	FString RootName;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("root_name"), RootName);
	}

	const FString NormalizedAssetPath = CortexST::NormalizeAssetPath(AssetPath);
	const FString AssetName = FPackageName::GetShortName(PackageName);
	const FString AssetFolder = FPackageName::GetLongPackagePath(PackageName);

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Create StateTree %s"), *AssetName)));

	UStateTreeFactory* Factory = NewObject<UStateTreeFactory>();
	Factory->SetSchemaClass(SchemaClass);

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* CreatedObject = AssetTools.CreateAsset(AssetName, AssetFolder, UStateTree::StaticClass(), Factory);
	UStateTree* StateTree = Cast<UStateTree>(CreatedObject);
	if (!StateTree)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to create StateTree asset: %s"), *AssetPath));
	}

	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
	if (!EditorData)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Created StateTree has no editor data: %s"), *AssetPath));
	}

	if (EditorData->SubTrees.IsEmpty())
	{
		if (!RootName.IsEmpty())
		{
			EditorData->AddSubTree(FName(*RootName));
		}
		else
		{
			EditorData->AddRootState();
		}
	}
	else if (!RootName.IsEmpty() && EditorData->SubTrees[0] != nullptr)
	{
		EditorData->SubTrees[0]->Modify();
		EditorData->SubTrees[0]->Name = FName(*RootName);
	}

	StateTree->MarkPackageDirty();

	if (CortexST::GetOptionalBool(Params, TEXT("save"), false))
	{
		FCortexCommandResult SaveResult = SaveAsset(NormalizedAssetPath);
		if (!SaveResult.bSuccess)
		{
			return SaveResult;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), NormalizedAssetPath);
	Data->SetBoolField(TEXT("created"), true);
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(StateTree));

	UE_LOG(LogCortexStateTree, Log, TEXT("Created StateTree: %s"), *NormalizedAssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexSTAssetOps::DuplicateAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	FString NewAssetPath;
	if (!CortexST::GetRequiredString(Params, TEXT("new_asset_path"), NewAssetPath, Error))
	{
		return Error;
	}

	FCortexSTAssetContext SourceContext;
	if (!CortexST::LoadAssetContext(AssetPath, SourceContext, Error))
	{
		return Error;
	}

	FString DestinationPackageName;
	if (!CortexST::ValidateWritablePackage(NewAssetPath, DestinationPackageName, Error))
	{
		return Error;
	}

	if (FindPackage(nullptr, *DestinationPackageName) || FPackageName::DoesPackageExist(DestinationPackageName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeAlreadyExists,
			FString::Printf(TEXT("StateTree already exists: %s"), *NewAssetPath));
	}

	const FString NormalizedDestinationPath = CortexST::NormalizeAssetPath(NewAssetPath);
	const FString DestinationName = FPackageName::GetShortName(DestinationPackageName);

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Duplicate StateTree %s"), *DestinationName)));

	UPackage* DestinationPackage = CreatePackage(*DestinationPackageName);
	UStateTree* DuplicatedTree = Cast<UStateTree>(
		StaticDuplicateObject(SourceContext.StateTree, DestinationPackage, FName(*DestinationName)));
	if (!DuplicatedTree)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to duplicate StateTree: %s"), *AssetPath));
	}

	FAssetRegistryModule::AssetCreated(DuplicatedTree);
	DestinationPackage->MarkPackageDirty();

	if (CortexST::GetOptionalBool(Params, TEXT("save"), false))
	{
		FCortexCommandResult SaveResult = SaveAsset(NormalizedDestinationPath);
		if (!SaveResult.bSuccess)
		{
			return SaveResult;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source"), SourceContext.AssetPath);
	Data->SetStringField(TEXT("new_asset_path"), NormalizedDestinationPath);
	Data->SetBoolField(TEXT("duplicated"), true);
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(DuplicatedTree));

	UE_LOG(LogCortexStateTree, Log, TEXT("Duplicated StateTree: %s -> %s"), *SourceContext.AssetPath, *NormalizedDestinationPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexSTAssetOps::DeleteAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}

	const bool bDryRun = CortexST::GetOptionalBool(Params, TEXT("dry_run"), false);
	const bool bForce = CortexST::GetOptionalBool(Params, TEXT("force"), false);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetIdentifier> Referencers;
	AssetRegistry.GetReferencers(FAssetIdentifier(Context.StateTree->GetOutermost()->GetFName()), Referencers);

	TSharedPtr<FJsonObject> ReferencerDetails = MakeShared<FJsonObject>();
	ReferencerDetails->SetArrayField(TEXT("referencers"), MakeReferencerArray(Referencers));

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("deleted"), false);
		Data->SetBoolField(TEXT("would_delete"), true);
		Data->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Data->SetArrayField(TEXT("referencers"), MakeReferencerArray(Referencers));
		return FCortexCommandRouter::Success(Data);
	}

	if (!CortexST::CheckExpectedFingerprint(Context.StateTree, Params, Error))
	{
		return Error;
	}

	if (Referencers.Num() > 0 && !bForce)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::HasReferences,
			FString::Printf(TEXT("StateTree has %d references. Use force=true to delete anyway."), Referencers.Num()),
			ReferencerDetails);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Delete StateTree %s"), *Context.StateTree->GetName())));

	UPackage* Package = Context.StateTree->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension());

	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
	{
		TryDeletePackageFile(PackageFilename);
	}

	UTextBuffer* DeleteGuard = nullptr;
	if (IsValid(Package))
	{
		DeleteGuard = NewObject<UTextBuffer>(Package, TEXT("__CortexDeleteGuard__"), RF_Public);
	}

	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(Context.StateTree);
	const int32 DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);

	if (DeleteGuard != nullptr)
	{
		DeleteGuard->ClearFlags(RF_Public | RF_Standalone);
	}

	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
	{
		TryDeletePackageFile(PackageFilename);
	}

	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to remove StateTree package file: %s"), *PackageFilename),
			ReferencerDetails);
	}

	if (DeletedCount == 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to delete StateTree: %s"), *Context.AssetPath),
			ReferencerDetails);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("deleted"), true);
	Data->SetStringField(TEXT("asset_path"), Context.AssetPath);

	UE_LOG(LogCortexStateTree, Log, TEXT("Deleted StateTree: %s"), *Context.AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexSTAssetOps::SaveAsset(const FString& AssetPath)
{
	UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *AssetPath);
	if (!StateTree)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeNotFound,
			FString::Printf(TEXT("StateTree not found: %s"), *AssetPath));
	}

	UPackage* Package = StateTree->GetOutermost();
	FString PackageFilename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
		Package->GetName(),
		PackageFilename,
		FPackageName::GetAssetPackageExtension()))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to resolve StateTree package filename: %s"), *Package->GetName()));
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, StateTree, *PackageFilename, SaveArgs))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to save StateTree: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("saved"), true);
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(StateTree));
	return FCortexCommandRouter::Success(Data);
}
