#include "Operations/CortexReflectOps.h"
#include "CortexReflectModule.h"
#include "UObject/UObjectIterator.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"

// Returns the full C++ name of a class (e.g. "AActor" not "Actor").
// UClass::GetName() strips the A/U prefix; GetPrefixCPP() gives it back.
static FString GetCppClassName(const UClass* Class)
{
	if (!Class)
	{
		return FString();
	}
	return FString(Class->GetPrefixCPP()) + Class->GetName();
}

UClass* FCortexReflectOps::FindClassByName(const FString& ClassName, FCortexCommandResult& OutError)
{
	if (ClassName.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("class_name is required")
		);
		return nullptr;
	}

	// 1. Try asset path (starts with /)
	if (ClassName.StartsWith(TEXT("/")))
	{
		FString PkgName = FPackageName::ObjectPathToPackageName(ClassName);
		if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::ClassNotFound,
				FString::Printf(TEXT("Blueprint asset not found: %s"), *ClassName)
			);
			return nullptr;
		}

		// Build full object path: /Game/Blueprints/BP_Foo -> /Game/Blueprints/BP_Foo.BP_Foo
		FString ObjectPath = ClassName;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			FString AssetName = FPackageName::GetShortName(ObjectPath);
			ObjectPath = ObjectPath + TEXT(".") + AssetName;
		}

		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		if (BP && BP->GeneratedClass)
		{
			return BP->GeneratedClass;
		}

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::ClassNotFound,
			FString::Printf(TEXT("Blueprint class not found: %s"), *ClassName)
		);
		return nullptr;
	}

	// 2. Resolve by name using TObjectIterator<UClass>.
	//
	//    UClass::GetName() returns the stripped name (e.g. "Actor" for AActor,
	//    "Character" for ACharacter, "Object" for UObject).
	//    GetPrefixCPP() returns the single-char prefix ("A" or "U").
	//
	//    We build a list of stripped names to try, in priority order:
	//      - If input starts with "A" or "U" (C++ prefix), strip it and try the stripped name first.
	//      - Always fall back to the input as-is (for classes that use the prefix in their actual name).
	//    NOTE: F-prefixed types are UStruct, never UClass — intentionally excluded.

	TArray<FString> StrippedCandidates;  // short names to compare against GetName()

	static const TCHAR* KnownPrefixes[] = { TEXT("A"), TEXT("U") };
	bool bStripped = false;
	for (const TCHAR* Prefix : KnownPrefixes)
	{
		if (ClassName.StartsWith(Prefix) && ClassName.Len() > 1)
		{
			FString Stripped = ClassName.Mid(1);
			StrippedCandidates.Add(Stripped);
			bStripped = true;
			break;
		}
	}

	// If no prefix was stripped, try adding common prefixes (bare "Actor" -> try "A"+"Actor" stripped = "Actor" already)
	// and add the bare name as the fallback.
	if (!bStripped)
	{
		// For bare name like "Actor", UClass::GetName() == "Actor", so exact match works directly.
		// But we also want "Actor" to resolve to AActor (GetName()=="Actor"), which is the exact match.
		// No prefix stripping needed — just search for the exact bare name.
	}

	// The bare name is always a candidate (either the already-stripped form or the original)
	StrippedCandidates.Add(ClassName);

	// Deduplicate: if stripping "AActor" gives "Actor" and we also add "AActor" (bare), remove duplicate.
	// This happens when both are the same string — TArray uniqueness isn't needed since names differ.

	// Single pass through all registered UClasses
	TMap<FString, UClass*> Found;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Candidate = *It;
		if (!IsValid(Candidate))
		{
			continue;
		}

		const FString& ShortName = Candidate->GetName();
		if (StrippedCandidates.Contains(ShortName) && !Found.Contains(ShortName))
		{
			Found.Add(ShortName, Candidate);
			if (Found.Num() == StrippedCandidates.Num())
			{
				break;
			}
		}
	}

	// Return first match in priority order
	for (const FString& Name : StrippedCandidates)
	{
		if (UClass** Result = Found.Find(Name))
		{
			return *Result;
		}
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::ClassNotFound,
		FString::Printf(TEXT("Class not found: %s"), *ClassName)
	);
	return nullptr;
}

bool FCortexReflectOps::IsProjectClass(const UClass* Class)
{
	return false;
}

FCortexCommandResult FCortexReflectOps::ClassHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexReflectOps::ClassDetail(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("class_name"), ClassName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("class_name parameter is required")
		);
	}

	FCortexCommandResult FindError;
	UClass* Class = FindClassByName(ClassName, FindError);
	if (!Class)
	{
		return FindError;
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	// Use the full C++ name (e.g. "AActor") not the stripped reflection name (e.g. "Actor")
	Result->SetStringField(TEXT("name"), GetCppClassName(Class));

	// Determine type
	UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Class);
	if (BPGC)
	{
		Result->SetStringField(TEXT("type"), TEXT("blueprint"));
		UBlueprint* BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
		if (BP)
		{
			Result->SetStringField(TEXT("asset_path"), BP->GetPathName());
		}
	}
	else
	{
		Result->SetStringField(TEXT("type"), TEXT("cpp"));
		// GetMetaData returns const FString& (NOT GetMetaDataText which doesn't exist)
		FString SourcePath = Class->GetMetaData(TEXT("ModuleRelativePath"));
		if (!SourcePath.IsEmpty())
		{
			Result->SetStringField(TEXT("source_path"), SourcePath);
		}
	}

	// Parent
	if (Class->GetSuperClass())
	{
		Result->SetStringField(TEXT("parent"), GetCppClassName(Class->GetSuperClass()));
	}

	// Module
	const FString* ModuleName = Class->FindMetaData(TEXT("ModuleName"));
	if (ModuleName)
	{
		Result->SetStringField(TEXT("module"), *ModuleName);
	}

	return FCortexCommandRouter::Success(Result);
}

FCortexCommandResult FCortexReflectOps::FindOverrides(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexReflectOps::FindUsages(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexReflectOps::Search(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}
