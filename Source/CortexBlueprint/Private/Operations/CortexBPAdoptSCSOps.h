#pragma once

#include "CoreMinimal.h"
#include "CortexGraphMemberReferences.h"
#include "Operations/CortexBPDependentScan.h"
#include "Operations/CortexBPSCSTemplateCopy.h"

class FJsonObject;
class UBlueprint;
struct FCortexCommandResult;

/**
 * blueprint.adopt_scs_into_native: hands an SCS component of the Blueprint over to a component the Blueprint inherits.
 *
 * Two routes, selected by where the inherited component lives. A component of a native parent is a default subobject of the
 * generated class CDO and is edited there (`native_cdo`); a component of a parent Blueprint's SCS is overridden through the
 * Blueprint's InheritableComponentHandler (`ich`). The data the SCS template carries (transform and properties, instanced
 * subobjects duplicated), the child SCS nodes and every reference (K2 variable nodes, component-bound events) move to the
 * inherited component; the SCS node is removed on request. Everything is read back fail-closed.
 *
 * Params: asset_path, mappings:[{scs_node, native_component}], copy?:[transform|properties|attach] (default all),
 * remove_scs?=false, dry_run?, expected_fingerprint?, compile?=true, save?=false, acknowledged_losses?
 * (`child_override:<blueprint>`, `property:<name>` and instanced sub-object keys).
 */
class FCortexBPAdoptSCSOps
{
public:
	struct FMappingRequest
	{
		FString ScsNode;
		FString NativeComponent;
	};

	struct FCopySet
	{
		bool bTransform = true;
		bool bProperties = true;
		bool bAttach = true;
	};

	struct FMappingPlan
	{
		FString ScsNode;
		FString NativeComponent;
		/** native_cdo | ich */
		FString Route;
		FString SourceClass;
		FString TargetClass;
		/** The SCS node has the name of the inherited component (a same-name collision the adoption resolves). */
		bool bSameName = false;
		/** Differences the copy applies: the requested categories only, sorted by property. */
		TArray<FCortexBPSCSTemplateCopy::FDiff> Transform;
		TArray<FCortexBPSCSTemplateCopy::FDiff> Properties;
		/** Differing instanced-subobject properties of the source. */
		TArray<FString> InstancedKeys;
		/** Differing properties the target class does not declare (a Blueprint component class's own variables). */
		TArray<FString> DroppedProperties;
		TArray<FString> ChildrenToReattach;
		TArray<FCortexMemberReference> Nodes;
		TArray<FGuid> BoundEvents;
		TArray<FCortexBPDependentHit> Dependents;
		/** Child Blueprints that hold an InheritableComponentHandler override of the node. */
		TArray<FString> ChildOverrides;
		/** Acknowledgment keys this mapping contributes. */
		TArray<FString> LossKeys;

		TSharedRef<FJsonObject> ToJson() const;
	};

	struct FRefusal
	{
		FString Code;
		FString Message;
		TSharedPtr<FJsonObject> Details;
	};

	struct FAdoptPlan
	{
		TArray<FMappingPlan> Mappings;
		/** Sorted and unique. */
		TArray<FString> RequiredAcknowledgment;
		TArray<FRefusal> Refusals;
		FCopySet Copy;
		bool bRemoveScs = false;

		/** {mappings, required_acknowledgment, copy, remove_scs, refusals}. Fields are added to Target. */
		void AppendTo(const TSharedRef<FJsonObject>& Target) const;
	};

	/** Read-only: never mutates the Blueprint. */
	static FAdoptPlan BuildPlan(UBlueprint* Blueprint, const TArray<FMappingRequest>& Mappings, const FCopySet& Copy, bool bRemoveScs);

	static FCortexCommandResult Execute(const TSharedPtr<FJsonObject>& Params);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook: runs inside the apply step after the references, template data and children moved (fault injection). */
	static void SetPostApplyHookForTest(TFunction<void(UBlueprint*)> Hook);
	static void ClearPostApplyHookForTest();
#endif
};
