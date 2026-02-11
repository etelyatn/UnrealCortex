// Copyright Andrei Sudarikov. All Rights Reserved.

#include "CortexBPAssetOps.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogCortexBP, Log, All);

namespace
{
	/** Helper: Load Blueprint from asset path */
	UBlueprint* LoadBlueprint(const FString& AssetPath, FString& OutError)
	{
		UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!LoadedAsset)
		{
			OutError = FString::Printf(TEXT("Blueprint not found at path: %s"), *AssetPath);
			return nullptr;
		}

		UBlueprint* BP = Cast<UBlueprint>(LoadedAsset);
		if (!BP)
		{
			OutError = FString::Printf(TEXT("Asset at path %s is not a Blueprint"), *AssetPath);
			return nullptr;
		}

		return BP;
	}

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

	// Check if asset already exists
	UObject* ExistingAsset = LoadObject<UBlueprint>(nullptr, *PackagePath);
	if (ExistingAsset)
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::BlueprintAlreadyExists;
		Result.ErrorMessage = FString::Printf(TEXT("Blueprint already exists at path: %s"), *PackagePath);
		return Result;
	}

	// Create the Blueprint
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

	// Mark package dirty (save via bp.save command)
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	// Return success with asset path
	Result.bSuccess = true;
	Result.Data = MakeShared<FJsonObject>();
	Result.Data->SetStringField(TEXT("asset_path"), PackagePath);
	Result.Data->SetStringField(TEXT("type"), TypeStr);
	Result.Data->SetBoolField(TEXT("created"), true);

	return Result;
}

FCortexCommandResult FCortexBPAssetOps::List(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = CortexErrorCodes::UnknownCommand;
	Result.ErrorMessage = TEXT("bp.list not implemented");
	return Result;
}

FCortexCommandResult FCortexBPAssetOps::GetInfo(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = CortexErrorCodes::UnknownCommand;
	Result.ErrorMessage = TEXT("bp.get_info not implemented");
	return Result;
}

FCortexCommandResult FCortexBPAssetOps::Delete(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = CortexErrorCodes::UnknownCommand;
	Result.ErrorMessage = TEXT("bp.delete not implemented");
	return Result;
}

FCortexCommandResult FCortexBPAssetOps::Duplicate(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = CortexErrorCodes::UnknownCommand;
	Result.ErrorMessage = TEXT("bp.duplicate not implemented");
	return Result;
}

FCortexCommandResult FCortexBPAssetOps::Compile(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = CortexErrorCodes::UnknownCommand;
	Result.ErrorMessage = TEXT("bp.compile not implemented");
	return Result;
}

FCortexCommandResult FCortexBPAssetOps::Save(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = CortexErrorCodes::UnknownCommand;
	Result.ErrorMessage = TEXT("bp.save not implemented");
	return Result;
}
