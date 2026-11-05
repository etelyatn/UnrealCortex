#include "Operations/CortexDataStructResolver.h"

#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** Loads a not-yet-loaded /Game struct by object path, skipping packages that do not exist. */
	UScriptStruct* LoadGameStruct(const FString& StructPath)
	{
		if (!StructPath.StartsWith(TEXT("/Game/")))
		{
			return nullptr;
		}

		FString PackageName;
		FString ObjectPath = StructPath;
		if (!StructPath.Split(TEXT("."), &PackageName, nullptr))
		{
			PackageName = StructPath;
			ObjectPath = FString::Printf(TEXT("%s.%s"), *StructPath, *FPackageName::GetShortName(StructPath));
		}

		if (!FPackageName::IsValidLongPackageName(PackageName) || !FPackageName::DoesPackageExist(PackageName))
		{
			return nullptr;
		}

		return LoadObject<UScriptStruct>(nullptr, *ObjectPath);
	}
}

bool CortexDataStructResolver::ResolveStructByName(const FString& StructName, UScriptStruct*& OutStruct, FString& OutError)
{
	const bool bHasFPrefix = StructName.StartsWith(TEXT("F"));
	const FString LookupName = bHasFPrefix ? StructName.RightChop(1) : StructName;
	const FString PrefixedName = bHasFPrefix ? StructName : TEXT("F") + StructName;

	OutStruct = FindObject<UScriptStruct>(nullptr, *StructName);
	if (OutStruct == nullptr)
	{
		OutStruct = FindObject<UScriptStruct>(nullptr, *LookupName);
	}
	if (OutStruct == nullptr)
	{
		OutStruct = FindFirstObjectSafe<UScriptStruct>(*StructName, EFindFirstObjectOptions::NativeFirst);
	}
	if (OutStruct == nullptr)
	{
		OutStruct = FindFirstObjectSafe<UScriptStruct>(*LookupName, EFindFirstObjectOptions::NativeFirst);
	}

	if (OutStruct == nullptr)
	{
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			const FString ItName = (*It)->GetName();
			if (ItName == StructName || ItName == LookupName || ItName == PrefixedName || (*It)->GetPathName() == StructName)
			{
				OutStruct = *It;
				break;
			}
		}
	}

	if (OutStruct == nullptr && StructName.StartsWith(TEXT("/")))
	{
		OutStruct = LoadGameStruct(StructName);
	}

	if (OutStruct == nullptr)
	{
		OutError = FString::Printf(TEXT("Struct not found: %s"), *StructName);
		return false;
	}

	return true;
}
