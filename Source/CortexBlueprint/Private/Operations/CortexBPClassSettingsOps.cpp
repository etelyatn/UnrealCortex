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

	// Try as C++ class name
	UClass* FoundClass = UClass::TryFindTypeSlow<UClass>(InterfacePath);

	// If user passed I-prefixed name (e.g., "IInteractable"), strip I and try U prefix
	if (!FoundClass && InterfacePath.Len() > 1
		&& InterfacePath[0] == TEXT('I') && FChar::IsUpper(InterfacePath[1]))
	{
		const FString BareName = InterfacePath.Mid(1);
		FoundClass = UClass::TryFindTypeSlow<UClass>(FString::Printf(TEXT("U%s"), *BareName));
		if (!FoundClass)
		{
			FoundClass = UClass::TryFindTypeSlow<UClass>(BareName);
		}
	}

	// Try with U prefix if bare name was given
	if (!FoundClass)
	{
		FoundClass = UClass::TryFindTypeSlow<UClass>(FString::Printf(TEXT("U%s"), *InterfacePath));
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
		const int32 ErrorCode = ResolveError.StartsWith(TEXT("NOT_INTERFACE:"))
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
		const int32 ErrorCode = ResolveError.StartsWith(TEXT("NOT_INTERFACE:"))
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
	// Stub — implemented in Task 4
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not yet implemented"));
}

FCortexCommandResult FCortexBPClassSettingsOps::SetReplicationSettings(const TSharedPtr<FJsonObject>& Params)
{
	// Stub — implemented in Task 5
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not yet implemented"));
}
