#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UBlueprint;
class UWidgetBlueprint;

/**
 * Discovery, scanning and reference renaming for `umg.rename_widget` with `scope:"dependents"`.
 *
 * Dependents are Blueprint descendants of the owner and Blueprints that reference the owner package.
 * They come from the Asset Registry (so unloaded assets are included) plus the loaded Blueprints the
 * registry cannot know about; each one is loaded behind a package-existence guard before it is scanned.
 */
namespace CortexUMGRenameDependents
{
	enum class ERole : uint8
	{
		Owner,
		Child,
		Referencer,
	};

	struct FReference
	{
		FString Graph;
		FGuid NodeGuid;
		FString NodeClass;
		/** The node itself, so the caller can record it in the transaction before the engine rename runs. */
		TWeakObjectPtr<UObject> Node;
	};

	struct FAssetPlan
	{
		FString AssetPath;
		ERole Role = ERole::Referencer;
		TWeakObjectPtr<UBlueprint> Blueprint;
		TSharedPtr<FJsonObject> Fingerprint;
		TArray<FReference> References;
		bool bWasDirty = false;
	};

	struct FConflict
	{
		FString AssetPath;
		FString Reason;
	};

	struct FPlan
	{
		/** Child and referencer Blueprints, sorted by asset path; the owner is planned by the caller. */
		TArray<FAssetPlan> Dependents;
		TArray<FConflict> Conflicts;
	};

	const TCHAR* RoleName(ERole Role);

	/** Discovers, loads, fingerprints and scans every dependent of `Owner` for references to `OldName`. */
	void BuildPlan(UWidgetBlueprint* Owner, FName OldName, FName NewName, FPlan& OutPlan);

	/** Lists graph nodes of `Blueprint` that reference the owner's `Name` member (variable nodes, component-bound events). */
	void ScanReferences(const UBlueprint* Blueprint, ERole Role, const UBlueprint* Owner, FName Name, TArray<FReference>& OutReferences);

	/** Renames the references in one dependent through the engine's variable-reference rename. */
	void RenameReferences(UWidgetBlueprint* Owner, UBlueprint* Dependent, ERole Role, FName OldName, FName NewName);

	TSharedPtr<FJsonObject> ReferenceToJson(const FReference& Reference);
	TSharedPtr<FJsonObject> AssetToJson(const FAssetPlan& Asset);
}
