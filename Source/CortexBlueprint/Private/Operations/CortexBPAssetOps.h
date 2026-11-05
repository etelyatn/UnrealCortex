#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "CortexCommandRouter.h"

class UBlueprint;
struct FAssetData;

/**
 * Blueprint asset operations
 */
class FCortexBPAssetOps
{
public:
	/**
	 * Create a new Blueprint asset
	 * Params: asset_path (string), type (string: Actor|Component|Widget|Interface|FunctionLibrary), force (bool, optional)
	 */
	static FCortexCommandResult Create(const TSharedPtr<FJsonObject>& Params);

	/**
	 * List Blueprint assets
	 * Params: path (string, optional), type (string, optional)
	 */
	static FCortexCommandResult List(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Get Blueprint info
	 * Params: asset_path (string)
	 */
	static FCortexCommandResult GetInfo(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Delete Blueprint asset
	 * Params: asset_path (string)
	 */
	static FCortexCommandResult Delete(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Duplicate Blueprint asset
	 * Params: asset_path (string), new_name (string), new_path (string, optional)
	 */
	static FCortexCommandResult Duplicate(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Compile Blueprint
	 * Params: asset_path (string)
	 * bSkipGarbageCollection: the caller compiles many assets and collects garbage once itself.
	 */
	static FCortexCommandResult Compile(const TSharedPtr<FJsonObject>& Params, bool bSkipGarbageCollection = false);

	/**
	 * Save Blueprint
	 * Params: asset_path (string)
	 */
	static FCortexCommandResult Save(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Rename/move a Blueprint asset and create redirector at old path.
	 * Params: source_path (string), dest_path (string)
	 */
	static FCortexCommandResult Rename(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Reparent a Blueprint to a new parent class
	 * Params: asset_path (string), new_parent (string - Blueprint path or C++ class name)
	 */
	static FCortexCommandResult Reparent(const TSharedPtr<FJsonObject>& Params);

	/** Load a Blueprint by asset path (with path normalization), returns nullptr and sets OutError if not found */
	static UBlueprint* LoadBlueprint(const FString& AssetPath, FString& OutError);

	/** Validate that a Blueprint asset path targets project-owned writable content for mutation commands. */
	static bool ValidateWritableBlueprintAssetPath(const FString& AssetPath, FString& OutError);

	/** Classify a loaded UBlueprint with the same rules used for Asset Registry rows (Interface, FunctionLibrary, MacroLibrary, AnimBlueprint, EditorUtility, Widget, Component, Actor, Object, Unknown) */
	static FString DetermineBlueprintType(const UBlueprint* BP);

	/** Immediate-parent display name shared by list/get_info/analyze/create: asset name for Blueprint generated classes (no "_C"), class name otherwise. Empty for null. */
	static FString ParentClassDisplayName(const UClass* ParentClass);

	/** Short name of the nearest native ancestor at or above ParentClass. Empty for null. */
	static FString NativeParentClassName(const UClass* ParentClass);

	/** Asset Registry class-path tag value to object path; empty for empty or "None" tags. */
	static FString ClassTagToObjectPath(const FString& TagValue);

	/** Determine the Blueprint type string from AssetRegistry tags, without loading the asset (same classifier as DetermineBlueprintType). */
	static FString DetermineBlueprintTypeFromAssetData(const FAssetData& AssetData);

	/** Canonical Blueprint type vocabulary in classification precedence order (shared by list, get_info, analyze_for_migration and compile_all). */
	static const TArray<FString>& GetBlueprintTypeNames();
};
