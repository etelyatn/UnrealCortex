#pragma once

#include "CoreMinimal.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

/** Shared Blueprint fixture helpers for the native member adoption test suites. */
namespace CortexAdoptTest
{
	/**
	 * Creates and compiles a transient-content Blueprint under /Game/Temp. PackageStem, when given,
	 * is used verbatim (suffixed with a random id) so two Blueprints can share an asset name.
	 */
	inline UBlueprint* MakeBlueprint(
		const TCHAR* AssetName,
		UClass* Parent,
		const TCHAR* PackageStem = nullptr,
		UClass* BlueprintClass = nullptr)
	{
		const FString Stem = PackageStem != nullptr ? FString(PackageStem) : FString(AssetName);
		UPackage* Package = CreatePackage(*FString::Printf(
			TEXT("/Game/Temp/%s_%s"), *Stem, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8)));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			Parent,
			Package,
			FName(AssetName),
			BPTYPE_Normal,
			BlueprintClass != nullptr ? BlueprintClass : UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
		if (Blueprint != nullptr)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
		}
		return Blueprint;
	}

	/** Marks the Blueprint and its generated classes garbage so short-name class lookups never see them again. */
	inline void Discard(UBlueprint* Blueprint)
	{
		if (Blueprint != nullptr)
		{
			if (Blueprint->GeneratedClass != nullptr)
			{
				Blueprint->GeneratedClass->MarkAsGarbage();
			}
			if (Blueprint->SkeletonGeneratedClass != nullptr)
			{
				Blueprint->SkeletonGeneratedClass->MarkAsGarbage();
			}
			Blueprint->MarkAsGarbage();
		}
	}
}
