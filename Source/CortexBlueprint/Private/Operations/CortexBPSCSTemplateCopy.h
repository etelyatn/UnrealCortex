#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FProperty;
class UActorComponent;
class UClass;
class UObject;

/**
 * Component template data moves shared by adopt_scs_into_native and swap_scs_component_class.
 *
 * A template's own data is what differs from its archetype. Differences are collected by property, filtered, and
 * applied to another template through the engine's own harvest copy (UEngine::CopyPropertiesForUnrelatedObjects), which
 * also duplicates instanced subobjects so the target never shares an object with the source.
 */
class FCortexBPSCSTemplateCopy
{
public:
	/** One property whose value differs from the archetype on the source template. */
	struct FDiff
	{
		FName Name;
		/** USceneComponent relative location, rotation or scale. */
		bool bTransform = false;
		/** The property is, or contains, an instanced subobject reference. */
		bool bInstanced = false;
		/** The target class declares a property of the same name and type. */
		bool bOnTarget = false;
		/** Declared by a Blueprint-generated class (a variable of the source's Blueprint component class). */
		bool bBlueprintDeclared = false;
		FString SourceText;
		FString TargetText;

		TSharedRef<FJsonObject> ToJson() const;
	};

	/**
	 * Differences of Source against its archetype, sorted by name. TargetClass decides bOnTarget; TargetOrNull supplies
	 * TargetText when it declares the property. Engine-owned placement data (attachment, creation method) is not template data.
	 */
	static void CollectDifferences(const UActorComponent* Source, const UClass* TargetClass, const UActorComponent* TargetOrNull, TArray<FDiff>& OutDiffs);

	/**
	 * Copies the Selected properties of Source onto Target; every other property of Target is left alone. Instanced
	 * subobjects among them are duplicated with Target as their outer.
	 */
	static bool CopySelected(UActorComponent* Source, UActorComponent* Target, const TSet<FName>& Selected, FString& OutError);

	/**
	 * Copies each named property of Source onto the property of the same name and type on Target, whether or not it differs from the
	 * archetype (a Blueprint component variable's default is not a difference but must still move). Instanced subobjects are CopySelected's.
	 */
	static bool CopyByName(const UActorComponent* Source, UActorComponent* Target, const TArray<FName>& Names, FString& OutError);

	/** True when the property of Container still holds its default-initialized value (nothing was set on it). */
	static bool IsInitialValue(const FProperty* Property, const UObject* Container);

	/** Content digest of an object's properties; instanced subobjects are hashed by class and content, never by name. */
	static FString Digest(const UObject* Object);
};
