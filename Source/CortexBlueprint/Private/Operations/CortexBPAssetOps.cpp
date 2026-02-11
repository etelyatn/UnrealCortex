// Copyright Andrei Sudarikov. All Rights Reserved.

#include "CortexBPAssetOps.h"
#include "CortexBlueprintModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet2/CompilerResultsLog.h"
#include "ScopedTransaction.h"

UBlueprint* FCortexBPAssetOps::LoadBlueprint(const FString& AssetPath, FString& OutError)
{
	// Normalize path (ensure it starts with /Game/)
	FString NormalizedPath = AssetPath;
	if (!NormalizedPath.StartsWith(TEXT("/")))
	{
		NormalizedPath = TEXT("/Game/") + NormalizedPath;
	}
	else if (!NormalizedPath.StartsWith(TEXT("/Game/")))
	{
		NormalizedPath = TEXT("/Game") + NormalizedPath;
	}

	// Check if package exists before LoadObject to avoid SkipPackage warnings
	FString PkgName = FPackageName::ObjectPathToPackageName(NormalizedPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FString::Printf(TEXT("Blueprint not found at path: %s"), *NormalizedPath);
		return nullptr;
	}

	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, *NormalizedPath);
	if (!LoadedAsset)
	{
		OutError = FString::Printf(TEXT("Blueprint not found at path: %s"), *NormalizedPath);
		return nullptr;
	}

	UBlueprint* BP = Cast<UBlueprint>(LoadedAsset);
	if (!BP)
	{
		OutError = FString::Printf(TEXT("Asset at path %s is not a Blueprint"), *NormalizedPath);
		return nullptr;
	}

	return BP;
}

namespace
{

	/** Helper: Determine parent class and blueprint class from type string */
	bool DetermineBlueprintType(
		const FString& TypeStr,
		UClass*& OutParentClass,
		TSubclassOf<UBlueprint>& OutBlueprintClass,
		FString& OutError)
	{
		if (TypeStr == TEXT("Actor"))
		{
			OutParentClass = AActor::StaticClass();
			OutBlueprintClass = UBlueprint::StaticClass();
			return true;
		}

		if (TypeStr == TEXT("Component"))
		{
			OutParentClass = UActorComponent::StaticClass();
			OutBlueprintClass = UBlueprint::StaticClass();
			return true;
		}

		if (TypeStr == TEXT("Widget"))
		{
			// Use dynamic class resolution to avoid compile-time dependency on UMG
			static UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
			static UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
			if (!UserWidgetClass || !WidgetBlueprintClass)
			{
				OutError = TEXT("Widget Blueprint classes not available (UMG module not loaded)");
				return false;
			}
			OutParentClass = UserWidgetClass;
			OutBlueprintClass = WidgetBlueprintClass;
			return true;
		}

		if (TypeStr == TEXT("Interface"))
		{
			OutParentClass = UInterface::StaticClass();
			OutBlueprintClass = UBlueprint::StaticClass();
			return true;
		}

		if (TypeStr == TEXT("FunctionLibrary"))
		{
			OutParentClass = UBlueprintFunctionLibrary::StaticClass();
			OutBlueprintClass = UBlueprint::StaticClass();
			return true;
		}

		OutError = FString::Printf(TEXT("Invalid Blueprint type: %s (supported: Actor, Component, Widget, Interface, FunctionLibrary)"), *TypeStr);
		return false;
	}

	/** Helper: Determine Blueprint type string from a loaded UBlueprint using class hierarchy */
	FString DetermineBlueprintType(const UBlueprint* BP)
	{
		if (!BP || !BP->ParentClass)
		{
			return TEXT("Unknown");
		}

		if (BP->BlueprintType == BPTYPE_Interface)
		{
			return TEXT("Interface");
		}

		if (BP->BlueprintType == BPTYPE_FunctionLibrary)
		{
			return TEXT("FunctionLibrary");
		}

		// Check for Widget using dynamic resolution to avoid compile-time dependency on UMG
		static UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
		if (UserWidgetClass && BP->ParentClass->IsChildOf(UserWidgetClass))
		{
			return TEXT("Widget");
		}

		if (BP->ParentClass->IsChildOf(UActorComponent::StaticClass()))
		{
			return TEXT("Component");
		}

		if (BP->ParentClass->IsChildOf(AActor::StaticClass()))
		{
			return TEXT("Actor");
		}

		return TEXT("Unknown");
	}
}

FCortexCommandResult FCortexBPAssetOps::Create(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;

	// Validate params
	if (!Params.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidField;
		Result.ErrorMessage = TEXT("Missing params object");
		return Result;
	}

	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidField;
		Result.ErrorMessage = TEXT("Missing or empty 'name' field");
		return Result;
	}

	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidField;
		Result.ErrorMessage = TEXT("Missing or empty 'path' field");
		return Result;
	}

	FString TypeStr;
	if (!Params->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidField;
		Result.ErrorMessage = TEXT("Missing or empty 'type' field");
		return Result;
	}

	// Determine Blueprint type
	UClass* ParentClass = nullptr;
	TSubclassOf<UBlueprint> BlueprintClass;
	FString TypeError;
	if (!DetermineBlueprintType(TypeStr, ParentClass, BlueprintClass, TypeError))
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidBlueprintType;
		Result.ErrorMessage = TypeError;
		return Result;
	}

	// Combine name and path to form package path
	FString PackagePath = FString::Printf(TEXT("%s/%s"), *Path, *Name);

	// Normalize path (ensure it starts with /Game/)
	if (!PackagePath.StartsWith(TEXT("/")))
	{
		PackagePath = TEXT("/Game/") + PackagePath;
	}
	else if (!PackagePath.StartsWith(TEXT("/Game/")))
	{
		PackagePath = TEXT("/Game") + PackagePath;
	}

	// Check if asset already exists (guard with FindPackage/DoesPackageExist to avoid warnings)
	FString ExistingPkgName = FPackageName::ObjectPathToPackageName(PackagePath);
	bool bPackageExists = FindPackage(nullptr, *ExistingPkgName) || FPackageName::DoesPackageExist(ExistingPkgName);
	UObject* ExistingAsset = bPackageExists ? LoadObject<UBlueprint>(nullptr, *PackagePath) : nullptr;
	if (ExistingAsset)
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::BlueprintAlreadyExists;
		Result.ErrorMessage = FString::Printf(TEXT("Blueprint already exists at path: %s"), *PackagePath);
		return Result;
	}

	// Create the Blueprint with undo/redo support
	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Create Blueprint %s"), *Name)
	));

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::SerializationError;
		Result.ErrorMessage = TEXT("Failed to create package");
		return Result;
	}

	EBlueprintType BPType = BPTYPE_Normal;
	if (TypeStr == TEXT("Interface"))
	{
		BPType = BPTYPE_Interface;
	}
	else if (TypeStr == TEXT("FunctionLibrary"))
	{
		BPType = BPTYPE_FunctionLibrary;
	}

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*Name),
		BPType,
		BlueprintClass,
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None
	);

	if (!NewBP)
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::SerializationError;
		Result.ErrorMessage = TEXT("Failed to create Blueprint");
		return Result;
	}

	// Save the Blueprint to disk
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	// Save the package
	const FString PackageFilename =
		FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	if (!UPackage::SavePackage(Package, NewBP, *PackageFilename, SaveArgs))
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::SerializationError;
		Result.ErrorMessage = FString::Printf(TEXT("Failed to save Blueprint package: %s"), *PackagePath);
		return Result;
	}

	// Return success with asset path
	Result.bSuccess = true;
	Result.Data = MakeShared<FJsonObject>();
	Result.Data->SetStringField(TEXT("asset_path"), PackagePath);
	Result.Data->SetStringField(TEXT("type"), TypeStr);
	Result.Data->SetStringField(TEXT("parent_class"), ParentClass ? ParentClass->GetName() : TEXT(""));
	Result.Data->SetBoolField(TEXT("created"), true);

	return Result;
}

FCortexCommandResult FCortexBPAssetOps::List(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;

	// Get optional path filter
	FString PathFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("path"), PathFilter);
	}

	// Query AssetRegistry for all Blueprint assets
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> BlueprintAssets;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	// Apply path filter if provided
	if (!PathFilter.IsEmpty())
	{
		// Normalize path (ensure it starts with /Game/)
		if (!PathFilter.StartsWith(TEXT("/")))
		{
			PathFilter = TEXT("/Game/") + PathFilter;
		}
		else if (!PathFilter.StartsWith(TEXT("/Game/")))
		{
			PathFilter = TEXT("/Game") + PathFilter;
		}

		Filter.PackagePaths.Add(FName(*PathFilter));
		Filter.bRecursivePaths = true;
	}

	AssetRegistry.GetAssets(Filter, BlueprintAssets);

	// Build result array
	TArray<TSharedPtr<FJsonValue>> BlueprintsArray;
	for (const FAssetData& AssetData : BlueprintAssets)
	{
		TSharedPtr<FJsonObject> BPObj = MakeShared<FJsonObject>();

		// Asset path
		BPObj->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());

		// Asset name
		BPObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());

		// Load Blueprint and determine type using proper class hierarchy
		UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
		FString Type = BP ? DetermineBlueprintType(BP) : TEXT("Unknown");

		BPObj->SetStringField(TEXT("type"), Type);

		BlueprintsArray.Add(MakeShared<FJsonValueObject>(BPObj));
	}

	// Return success with blueprints array
	Result.bSuccess = true;
	Result.Data = MakeShared<FJsonObject>();
	Result.Data->SetArrayField(TEXT("blueprints"), BlueprintsArray);
	Result.Data->SetNumberField(TEXT("count"), BlueprintsArray.Num());

	return Result;
}

FCortexCommandResult FCortexBPAssetOps::GetInfo(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;

	// Validate params
	if (!Params.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidField;
		Result.ErrorMessage = TEXT("Missing params object");
		return Result;
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidField;
		Result.ErrorMessage = TEXT("Missing or empty 'asset_path' field");
		return Result;
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprint(AssetPath, LoadError);
	if (!BP)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	// Build info object
	TSharedPtr<FJsonObject> InfoObj = MakeShared<FJsonObject>();

	// Basic info
	InfoObj->SetStringField(TEXT("name"), BP->GetName());
	InfoObj->SetStringField(TEXT("asset_path"), AssetPath);

	// Determine type using shared helper
	FString Type = DetermineBlueprintType(BP);
	InfoObj->SetStringField(TEXT("type"), Type);

	// Parent class
	if (BP->ParentClass)
	{
		InfoObj->SetStringField(TEXT("parent_class"), BP->ParentClass->GetName());
	}

	// Compilation status
	bool bIsCompiled = (BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);
	InfoObj->SetBoolField(TEXT("is_compiled"), bIsCompiled);

	// Variables
	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	for (FBPVariableDescription& Variable : BP->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Variable.VarName.ToString());
		VarObj->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());

		// Add default value if available
		if (!Variable.DefaultValue.IsEmpty())
		{
			VarObj->SetStringField(TEXT("default_value"), Variable.DefaultValue);
		}

		// Add exposure status
		bool bIsExposed = (Variable.PropertyFlags & CPF_BlueprintVisible) != 0;
		VarObj->SetBoolField(TEXT("is_exposed"), bIsExposed);

		VariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
	}
	InfoObj->SetArrayField(TEXT("variables"), VariablesArray);

	// Functions (use Blueprint->FunctionGraphs instead of GetAllGraphs)
	TArray<TSharedPtr<FJsonValue>> FunctionsArray;
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph)
		{
			TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
			FuncObj->SetStringField(TEXT("name"), Graph->GetName());
			FunctionsArray.Add(MakeShared<FJsonValueObject>(FuncObj));
		}
	}
	InfoObj->SetArrayField(TEXT("functions"), FunctionsArray);

	// Graphs (all graphs with node counts)
	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph)
		{
			TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
			GraphObj->SetStringField(TEXT("name"), Graph->GetName());
			GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
		}
	}
	InfoObj->SetArrayField(TEXT("graphs"), GraphsArray);

	// Return success
	Result.bSuccess = true;
	Result.Data = InfoObj;

	return Result;
}

FCortexCommandResult FCortexBPAssetOps::Delete(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	// Load blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintNotFound,
			LoadError
		);
	}

	// Check for references unless force is true
	if (!bForce)
	{
		IAssetRegistry& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FAssetIdentifier> Referencers;
		AssetRegistry.GetReferencers(
			FAssetIdentifier(Blueprint->GetOutermost()->GetFName()),
			Referencers
		);

		if (Referencers.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> RefArray;
			for (const FAssetIdentifier& Ref : Referencers)
			{
				RefArray.Add(MakeShared<FJsonValueString>(Ref.ToString()));
			}
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("references"), RefArray);

			return FCortexCommandRouter::Error(
				CortexErrorCodes::HasReferences,
				FString::Printf(TEXT("Blueprint has %d references. Use force=true to delete anyway."),
					Referencers.Num()),
				Details
			);
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Delete Blueprint %s"), *Blueprint->GetName())
	));

	UPackage* Package = Blueprint->GetOutermost();

	// Remove from asset registry
	FAssetRegistryModule::AssetDeleted(Blueprint);

	// Mark for garbage collection
	Blueprint->ClearFlags(RF_Standalone | RF_Public);
	Blueprint->MarkAsGarbage();
	Package->MarkAsGarbage();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("deleted"), true);
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Deleted Blueprint: %s"), *AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPAssetOps::Duplicate(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NewName;

	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, new_name")
		);
	}

	// Load source blueprint
	FString LoadError;
	UBlueprint* SourceBP = LoadBlueprint(AssetPath, LoadError);
	if (SourceBP == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintNotFound,
			LoadError
		);
	}

	// Determine destination path
	FString NewPath;
	Params->TryGetStringField(TEXT("new_path"), NewPath);
	if (NewPath.IsEmpty())
	{
		// Default to same directory as source
		NewPath = FPackageName::GetLongPackagePath(SourceBP->GetOutermost()->GetName());
	}

	FString NewPackagePath = FString::Printf(TEXT("%s/%s"), *NewPath, *NewName);

	// Check if destination already exists
	UPackage* ExistingPackage = FindPackage(nullptr, *NewPackagePath);
	if (ExistingPackage != nullptr)
	{
		UObject* ExistingAsset = StaticFindObject(UBlueprint::StaticClass(), ExistingPackage, *NewName);
		if (ExistingAsset != nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::BlueprintAlreadyExists,
				FString::Printf(TEXT("Asset already exists: %s"), *NewPackagePath)
			);
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Duplicate Blueprint %s"), *NewName)
	));

	// Create destination package
	UPackage* NewPackage = CreatePackage(*NewPackagePath);

	// Duplicate the object
	UBlueprint* NewBP = Cast<UBlueprint>(
		StaticDuplicateObject(SourceBP, NewPackage, FName(*NewName))
	);

	if (NewBP == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			TEXT("Failed to duplicate Blueprint")
		);
	}

	FAssetRegistryModule::AssetCreated(NewBP);
	NewPackage->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source"), AssetPath);
	Data->SetStringField(TEXT("new_asset_path"), NewPackagePath);
	Data->SetBoolField(TEXT("duplicated"), true);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Duplicated %s -> %s"), *AssetPath, *NewPackagePath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPAssetOps::Compile(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	// Load blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintNotFound,
			LoadError
		);
	}

	// Compile
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	bool bCompiled = (Blueprint->Status == BS_UpToDate
		|| Blueprint->Status == BS_UpToDateWithWarnings);

	if (!bCompiled)
	{
		// Collect errors from nodes
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		TArray<TSharedPtr<FJsonValue>> WarningsArray;

		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);

		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node == nullptr || !Node->bHasCompilerMessage)
				{
					continue;
				}

				TSharedRef<FJsonObject> MsgObj = MakeShared<FJsonObject>();
				MsgObj->SetStringField(TEXT("node"), Node->GetName());
				MsgObj->SetStringField(TEXT("message"), Node->ErrorMsg);

				if (Node->ErrorType <= EMessageSeverity::Error)
				{
					ErrorsArray.Add(MakeShared<FJsonValueObject>(MsgObj));
				}
				else
				{
					WarningsArray.Add(MakeShared<FJsonValueObject>(MsgObj));
				}
			}
		}

		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("errors"), ErrorsArray);
		Details->SetArrayField(TEXT("warnings"), WarningsArray);

		return FCortexCommandRouter::Error(
			CortexErrorCodes::CompileFailed,
			FString::Printf(TEXT("Blueprint compilation failed with %d errors"), ErrorsArray.Num()),
			Details
		);
	}

	// Collect warnings even on success
	TArray<TSharedPtr<FJsonValue>> WarningsArray;
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph == nullptr)
		{
			continue;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node != nullptr && Node->bHasCompilerMessage
				&& Node->ErrorType > EMessageSeverity::Error)
			{
				TSharedRef<FJsonObject> WarnObj = MakeShared<FJsonObject>();
				WarnObj->SetStringField(TEXT("node"), Node->GetName());
				WarnObj->SetStringField(TEXT("message"), Node->ErrorMsg);
				WarningsArray.Add(MakeShared<FJsonValueObject>(WarnObj));
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("compiled"), true);
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("warnings"), WarningsArray);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Compiled Blueprint: %s"), *AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPAssetOps::Save(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	// Load blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintNotFound,
			LoadError
		);
	}

	UPackage* Package = Blueprint->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs);

	if (!bSaved)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to save Blueprint: %s"), *AssetPath)
		);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("saved"), true);
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Saved Blueprint: %s"), *AssetPath);

	return FCortexCommandRouter::Success(Data);
}
