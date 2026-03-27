#include "Operations/CortexBPClassSettingsOps.h"

#include "Operations/CortexBPAssetOps.h"
#include "CortexBlueprintModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"
#include "UObject/TopLevelAssetPath.h"

UClass* FCortexBPClassSettingsOps::ResolveInterfaceClass(const FString& InterfacePath, FString& OutError)
{
	if (InterfacePath.IsEmpty())
	{
		OutError = TEXT("Interface path is empty");
		return nullptr;
	}

	// Try as a Blueprint interface asset path first
	if (InterfacePath.StartsWith(TEXT("/")) || InterfacePath.Contains(TEXT(".")))
	{
		FString NormalizedPath = InterfacePath;
		if (NormalizedPath.EndsWith(TEXT("_C")))
		{
			NormalizedPath.LeftChopInline(2);
		}

		const FString PkgName = FPackageName::ObjectPathToPackageName(NormalizedPath);
		if (FindPackage(nullptr, *PkgName) || FPackageName::DoesPackageExist(PkgName))
		{
			UBlueprint* InterfaceBP = LoadObject<UBlueprint>(nullptr, *NormalizedPath);
			if (InterfaceBP && InterfaceBP->GeneratedClass)
			{
				if (InterfaceBP->GeneratedClass->HasAnyClassFlags(CLASS_Interface))
				{
					return InterfaceBP->GeneratedClass;
				}
				OutError = FString::Printf(TEXT("'%s' is not an interface Blueprint"), *InterfacePath);
				return nullptr;
			}
		}
	}

	// Try as C++ class name using FindFirstObject (searches all packages by short name)
	UClass* FoundClass = FindFirstObject<UClass>(*InterfacePath, EFindFirstObjectOptions::NativeFirst);

	// If user passed I-prefixed name (e.g., "IInteractable"), strip I and try bare name
	if (!FoundClass && InterfacePath.Len() > 1
		&& InterfacePath[0] == TEXT('I') && FChar::IsUpper(InterfacePath[1]))
	{
		const FString BareName = InterfacePath.Mid(1);
		FoundClass = FindFirstObject<UClass>(*BareName, EFindFirstObjectOptions::NativeFirst);
	}

	// Try with U prefix stripped (UBlendableInterface -> BlendableInterface)
	if (!FoundClass && InterfacePath.StartsWith(TEXT("U")))
	{
		FoundClass = FindFirstObject<UClass>(*InterfacePath.Mid(1), EFindFirstObjectOptions::NativeFirst);
	}

	if (FoundClass)
	{
		if (FoundClass->HasAnyClassFlags(CLASS_Interface))
		{
			return FoundClass;
		}
		OutError = FString::Printf(TEXT("NOT_INTERFACE:Class '%s' exists but is not an interface"), *InterfacePath);
		return nullptr;
	}

	OutError = FString::Printf(TEXT("Could not resolve interface class '%s'"), *InterfacePath);
	return nullptr;
}

FCortexCommandResult FCortexBPClassSettingsOps::AddInterface(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
	}

	FString InterfacePath;
	if (!Params->TryGetStringField(TEXT("interface_path"), InterfacePath) || InterfacePath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'interface_path' field"));
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	FString ResolveError;
	UClass* InterfaceClass = ResolveInterfaceClass(InterfacePath, ResolveError);
	if (!InterfaceClass)
	{
		const FString ErrorCode = ResolveError.StartsWith(TEXT("NOT_INTERFACE:"))
			? CortexErrorCodes::InvalidOperation
			: CortexErrorCodes::ClassNotFound;
		const FString CleanError = ResolveError.StartsWith(TEXT("NOT_INTERFACE:"))
			? ResolveError.Mid(14)
			: ResolveError;
		return FCortexCommandRouter::Error(ErrorCode, CleanError);
	}

	// Check if already implemented
	const FString InterfaceClassPath = InterfaceClass->GetPathName();
	for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
	{
		if (Desc.Interface && Desc.Interface->GetPathName() == InterfaceClassPath)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Blueprint already implements interface '%s'"),
					*InterfaceClass->GetName()));
		}
	}

	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Add Interface %s to %s"),
				*InterfaceClass->GetName(), *Blueprint->GetName())));

		FBlueprintEditorUtils::ImplementNewInterface(Blueprint, FTopLevelAssetPath(InterfaceClass));
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	bool bDidCompile = false;
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		bDidCompile = (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	}

	// Gather stub functions created
	TArray<TSharedPtr<FJsonValue>> StubFunctions;
	for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
	{
		if (Desc.Interface && Desc.Interface->GetPathName() == InterfaceClassPath)
		{
			for (const UEdGraph* Graph : Desc.Graphs)
			{
				if (Graph)
				{
					StubFunctions.Add(MakeShared<FJsonValueString>(Graph->GetName()));
				}
			}
			break;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("interface_name"), InterfaceClass->GetName());
	Data->SetStringField(TEXT("interface_path"), InterfaceClassPath);
	Data->SetBoolField(TEXT("compiled"), bCompile && bDidCompile);
	Data->SetArrayField(TEXT("stub_functions"), StubFunctions);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Added interface %s to %s"),
		*InterfaceClass->GetName(), *AssetPath);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPClassSettingsOps::RemoveInterface(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
	}

	FString InterfacePath;
	if (!Params->TryGetStringField(TEXT("interface_path"), InterfacePath) || InterfacePath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'interface_path' field"));
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	FString ResolveError;
	UClass* InterfaceClass = ResolveInterfaceClass(InterfacePath, ResolveError);
	if (!InterfaceClass)
	{
		const FString ErrorCode = ResolveError.StartsWith(TEXT("NOT_INTERFACE:"))
			? CortexErrorCodes::InvalidOperation
			: CortexErrorCodes::ClassNotFound;
		const FString CleanError = ResolveError.StartsWith(TEXT("NOT_INTERFACE:"))
			? ResolveError.Mid(14)
			: ResolveError;
		return FCortexCommandRouter::Error(ErrorCode, CleanError);
	}

	// Check if actually implemented
	const FString InterfaceClassPath = InterfaceClass->GetPathName();
	bool bFound = false;
	TArray<FString> RemovedGraphNames;
	for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
	{
		if (Desc.Interface && Desc.Interface->GetPathName() == InterfaceClassPath)
		{
			bFound = true;
			for (const UEdGraph* Graph : Desc.Graphs)
			{
				if (Graph)
				{
					RemovedGraphNames.Add(Graph->GetName());
				}
			}
			break;
		}
	}

	if (!bFound)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Blueprint does not implement interface '%s'"),
				*InterfaceClass->GetName()));
	}

	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Remove Interface %s from %s"),
				*InterfaceClass->GetName(), *Blueprint->GetName())));

		FBlueprintEditorUtils::RemoveInterface(Blueprint, FTopLevelAssetPath(InterfaceClass));
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	bool bDidCompile = false;
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		bDidCompile = (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	}

	TArray<TSharedPtr<FJsonValue>> RemovedGraphsArray;
	for (const FString& GraphName : RemovedGraphNames)
	{
		RemovedGraphsArray.Add(MakeShared<FJsonValueString>(GraphName));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("interface_name"), InterfaceClass->GetName());
	Data->SetStringField(TEXT("interface_path"), InterfaceClassPath);
	Data->SetBoolField(TEXT("compiled"), bCompile && bDidCompile);
	Data->SetArrayField(TEXT("removed_graphs"), RemovedGraphsArray);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Removed interface %s from %s"),
		*InterfaceClass->GetName(), *AssetPath);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPClassSettingsOps::SetTickSettings(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	if (!Blueprint->GeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::CompileFailed, TEXT("Blueprint has no GeneratedClass"));
	}

	AActor* ActorCDO = Cast<AActor>(GeneratedClass->GetDefaultObject(false));
	if (!ActorCDO)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidBlueprintType,
			TEXT("Blueprint CDO is not an AActor — tick settings only apply to Actor Blueprints"));
	}

	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Set Tick Settings on %s"), *Blueprint->GetName())));
		ActorCDO->Modify();

		bool bTempBool = false;
		if (Params->TryGetBoolField(TEXT("can_ever_tick"), bTempBool))
		{
			ActorCDO->PrimaryActorTick.bCanEverTick = bTempBool;
		}

		if (Params->TryGetBoolField(TEXT("start_with_tick_enabled"), bTempBool))
		{
			ActorCDO->PrimaryActorTick.bStartWithTickEnabled = bTempBool;
			if (bTempBool)
			{
				ActorCDO->PrimaryActorTick.bCanEverTick = true;
			}
		}

		double TempDouble = 0.0;
		if (Params->TryGetNumberField(TEXT("tick_interval"), TempDouble))
		{
			ActorCDO->PrimaryActorTick.TickInterval = static_cast<float>(TempDouble);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	// Capture tick values before compile — PrimaryActorTick is not a UPROPERTY
	// on AActor, so CPFUO won't preserve its members across recompile.
	const bool bSavedCanEverTick = ActorCDO->PrimaryActorTick.bCanEverTick;
	const bool bSavedStartWithTickEnabled = ActorCDO->PrimaryActorTick.bStartWithTickEnabled;
	const float SavedTickInterval = ActorCDO->PrimaryActorTick.TickInterval;

	bool bDidCompile = false;
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		bDidCompile = (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

		// Re-apply tick values to the new CDO created by compilation
		GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		if (GeneratedClass)
		{
			ActorCDO = Cast<AActor>(GeneratedClass->GetDefaultObject(false));
			if (ActorCDO)
			{
				ActorCDO->PrimaryActorTick.bCanEverTick = bSavedCanEverTick;
				ActorCDO->PrimaryActorTick.bStartWithTickEnabled = bSavedStartWithTickEnabled;
				ActorCDO->PrimaryActorTick.TickInterval = SavedTickInterval;
			}
		}
	}

	bool bDidSave = false;
	if (bSave)
	{
		UPackage* Package = Blueprint->GetOutermost();
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		bDidSave = UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("start_with_tick_enabled"), ActorCDO->PrimaryActorTick.bStartWithTickEnabled);
	Data->SetBoolField(TEXT("can_ever_tick"), ActorCDO->PrimaryActorTick.bCanEverTick);
	Data->SetNumberField(TEXT("tick_interval"), ActorCDO->PrimaryActorTick.TickInterval);
	Data->SetBoolField(TEXT("compiled"), bCompile && bDidCompile);
	Data->SetBoolField(TEXT("saved"), bSave && bDidSave);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Set tick settings on %s"), *AssetPath);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPClassSettingsOps::SetReplicationSettings(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);

	FString DormancyString;
	const bool bHasDormancy = Params->TryGetStringField(TEXT("net_dormancy"), DormancyString);
	ENetDormancy DormancyValue = ENetDormancy::DORM_Never;
	if (bHasDormancy)
	{
		if (DormancyString == TEXT("DORM_Never")) { DormancyValue = ENetDormancy::DORM_Never; }
		else if (DormancyString == TEXT("DORM_Awake")) { DormancyValue = ENetDormancy::DORM_Awake; }
		else if (DormancyString == TEXT("DORM_DormantAll")) { DormancyValue = ENetDormancy::DORM_DormantAll; }
		else if (DormancyString == TEXT("DORM_DormantPartial")) { DormancyValue = ENetDormancy::DORM_DormantPartial; }
		else if (DormancyString == TEXT("DORM_Initial")) { DormancyValue = ENetDormancy::DORM_Initial; }
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidValue,
				FString::Printf(TEXT("Invalid net_dormancy value '%s'. Valid: DORM_Never, DORM_Awake, DORM_DormantAll, DORM_DormantPartial, DORM_Initial"),
					*DormancyString));
		}
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	if (!Blueprint->GeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::CompileFailed, TEXT("Blueprint has no GeneratedClass"));
	}

	AActor* ActorCDO = Cast<AActor>(GeneratedClass->GetDefaultObject(false));
	if (!ActorCDO)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidBlueprintType,
			TEXT("Blueprint CDO is not an AActor — replication settings only apply to Actor Blueprints"));
	}

	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Set Replication Settings on %s"), *Blueprint->GetName())));
		ActorCDO->Modify();

		bool bTempBool = false;
		if (Params->TryGetBoolField(TEXT("replicates"), bTempBool))
		{
			ActorCDO->SetReplicates(bTempBool);
		}

		if (Params->TryGetBoolField(TEXT("replicate_movement"), bTempBool))
		{
			ActorCDO->SetReplicateMovement(bTempBool);
		}

		if (bHasDormancy)
		{
			ActorCDO->NetDormancy = DormancyValue;
		}

		if (Params->TryGetBoolField(TEXT("net_use_owner_relevancy"), bTempBool))
		{
			ActorCDO->bNetUseOwnerRelevancy = bTempBool;
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	bool bDidCompile = false;
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		bDidCompile = (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	}

	bool bDidSave = false;
	if (bSave)
	{
		UPackage* Package = Blueprint->GetOutermost();
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		bDidSave = UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs);
	}

	auto DormancyToString = [](ENetDormancy D) -> FString
	{
		switch (D)
		{
		case ENetDormancy::DORM_Never: return TEXT("DORM_Never");
		case ENetDormancy::DORM_Awake: return TEXT("DORM_Awake");
		case ENetDormancy::DORM_DormantAll: return TEXT("DORM_DormantAll");
		case ENetDormancy::DORM_DormantPartial: return TEXT("DORM_DormantPartial");
		case ENetDormancy::DORM_Initial: return TEXT("DORM_Initial");
		default: return TEXT("Unknown");
		}
	};

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("replicates"), ActorCDO->GetIsReplicated());
	Data->SetBoolField(TEXT("replicate_movement"), ActorCDO->IsReplicatingMovement());
	Data->SetStringField(TEXT("net_dormancy"), DormancyToString(ActorCDO->NetDormancy));
	Data->SetBoolField(TEXT("net_use_owner_relevancy"), ActorCDO->bNetUseOwnerRelevancy);
	Data->SetBoolField(TEXT("compiled"), bCompile && bDidCompile);
	Data->SetBoolField(TEXT("saved"), bSave && bDidSave);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Set replication settings on %s"), *AssetPath);
	return FCortexCommandRouter::Success(Data);
}
