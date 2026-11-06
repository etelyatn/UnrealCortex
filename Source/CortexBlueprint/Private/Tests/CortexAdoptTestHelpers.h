#pragma once

#include "CoreMinimal.h"

#include "Editor.h"
#include "Editor/Transactor.h"
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

	/**
	 * GC-safety invariant for every migration fixture: the guarded runs, undo and rollback leave editor transaction records that
	 * name the fixture's classes, graphs and reinstanced instances. Retiring (MarkAsGarbage) those objects while the records are
	 * alive lets a later garbage collection scan freed objects and crash a random test, so the undo/redo history is emptied FIRST.
	 */
	inline void ResetUndoHistory()
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("Retire migration test fixture")));
		}
	}

	/** Marks the Blueprint and its generated classes garbage so short-name class lookups never see them again. */
	inline void Discard(UBlueprint* Blueprint)
	{
		if (Blueprint != nullptr)
		{
			ResetUndoHistory();
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
