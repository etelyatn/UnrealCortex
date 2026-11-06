#pragma once

#include "CoreMinimal.h"
#include "CortexGraphMemberReferences.h"
#include "Operations/CortexBPDependentScan.h"
#include "Operations/CortexBPSCSTemplateCopy.h"

class FJsonObject;
class UBlueprint;
struct FCortexCommandResult;

/**
 * blueprint.swap_scs_component_class: replaces the component class of one of the Blueprint's SCS nodes.
 *
 * The replacement node is built through USimpleConstructionScript::CreateNode and takes over the identity of the old one
 * (variable name, VariableGuid, category, metadata, attach name, parent link, children, position among its siblings); the template
 * data moves by property name: properties of the shared native ancestry are kept, Blueprint component variables with a counterpart
 * on the new class move their value, instanced subobjects are duplicated, everything else is dropped and, when it carried a
 * non-default value, must be acknowledged. The old node is removed last; the result is read back fail-closed.
 *
 * Params: asset_path, component_name, new_class, dry_run?, expected_fingerprint?, compile?=true, save?=false,
 * acknowledged_losses? (`property:<name>` keys).
 */
class FCortexBPSwapSCSClassOps
{
public:
	/** A property that does not survive the swap. */
	struct FDropped
	{
		FString Property;
		FString Value;
		/** The value is not the type's initial value, so the caller must acknowledge the loss. */
		bool bNeedsAcknowledgment = false;
	};

	/** A Blueprint component variable whose value moves to the same-named property of the new class. */
	struct FMoved
	{
		FName Property;
		FString Value;
	};

	struct FRefusal
	{
		FString Code;
		FString Message;
		TSharedPtr<FJsonObject> Details;
	};

	struct FSwapPlan
	{
		FString ComponentName;
		FString OldClass;
		FString NewClass;
		/** The first native class of the old class's chain; the new class must derive from it. */
		FString OldNativeAncestor;
		/** Differences that are native-ancestor data the new class declares too (copied as they are). */
		TArray<FCortexBPSCSTemplateCopy::FDiff> Kept;
		TArray<FMoved> Moved;
		/** Names of the differing properties copied through the harvest copy (Kept and the instanced ones among Moved). */
		TArray<FName> Selected;
		TArray<FDropped> Dropped;
		TArray<FString> InstancedKeys;
		TArray<FString> Children;
		TArray<FCortexMemberReference> Nodes;
		TArray<FGuid> BoundEvents;
		TArray<FCortexBPDependentHit> Dependents;
		TArray<FString> ChildOverrides;
		/** Sorted and unique: property:<name>. */
		TArray<FString> RequiredAcknowledgment;
		TArray<FRefusal> Refusals;

		/** Fields are added to Target. */
		void AppendTo(const TSharedRef<FJsonObject>& Target) const;
	};

	/** Read-only: never mutates the Blueprint. */
	static FSwapPlan BuildPlan(UBlueprint* Blueprint, const FString& ComponentName, UClass* NewClass);

	static FCortexCommandResult Execute(const TSharedPtr<FJsonObject>& Params);
};
