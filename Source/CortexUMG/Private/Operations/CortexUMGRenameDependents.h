#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UBlueprint;
class UWidget;
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

	/** One generated member the engine rename moves together with the widget: the widget itself, then each UIComponent member. */
	struct FRenamedMember
	{
		FName OldName;
		FName NewName;
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
		/** EBlueprintStatus of the Blueprint when planned. */
		uint8 OriginalStatus = 0;
		/** The Blueprint's generated class when planned; a different one means it was recompiled during the request. */
		TWeakObjectPtr<UClass> GeneratedClass;
		/** Hash of every variable node's (guid, member name): proves an unmodified dependent was not edited. */
		uint32 VariableNodeSignature = 0;
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

	/** The widget's own member plus every UIComponent member the engine rename propagates, with their new names. */
	void CollectRenamedMembers(UWidgetBlueprint* Owner, UWidget* Widget, FName NewName, TArray<FRenamedMember>& OutMembers);

	/** Discovers, loads, fingerprints and scans every dependent of `Owner` for references to the renamed members. */
	void BuildPlan(UWidgetBlueprint* Owner, const TArray<FRenamedMember>& Members, FPlan& OutPlan);

	/** Lists graph nodes of `Blueprint` that reference one of the owner's renamed members (variable nodes, component-bound events). */
	void ScanReferences(const UBlueprint* Blueprint, ERole Role, const UBlueprint* Owner, const TArray<FRenamedMember>& Members, TArray<FReference>& OutReferences);

	/** Renames the references in one dependent through the engine's variable-reference rename. */
	void RenameReferences(UWidgetBlueprint* Owner, UBlueprint* Dependent, ERole Role, const TArray<FRenamedMember>& Members);

	/** Hash of every variable node's (guid, member name) in `Blueprint`. */
	uint32 ComputeVariableNodeSignature(const UBlueprint* Blueprint);

	/** Returns a dependent, whose content is verified as planned, to its planned status unless it was recompiled meanwhile. */
	void RestorePlannedStatus(const FAssetPlan& Asset);

	TSharedPtr<FJsonObject> ReferenceToJson(const FReference& Reference);
	TSharedPtr<FJsonObject> AssetToJson(const FAssetPlan& Asset);

#if WITH_AUTOMATION_TESTS
	/**
	 * Test-only faults injected after the engine rename. Never reachable from command input.
	 * `leftover_reference`: the rescan reports one synthetic leftover reference.
	 * `dirty_unmodified_dependent`: a planned dependent without references is marked modified (dirty, Status BS_Dirty).
	 * `edit_unmodified_dependent`: a planned dependent without references has one variable node edited.
	 */
	void SetFaultForTesting(FName Fault);
#endif

	/** True when `Fault` is the active test fault; always false in shipping builds. */
	bool IsFaultInjected(FName Fault);

	/** Applies the `dirty_unmodified_dependent` / `edit_unmodified_dependent` faults to the plan's reference-free dependents. */
	void InjectUnmodifiedDependentFaults(const TArray<FAssetPlan>& Dependents);
}
