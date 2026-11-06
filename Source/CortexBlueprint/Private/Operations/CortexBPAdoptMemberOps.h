#pragma once

#include "CoreMinimal.h"
#include "CortexGraphMemberReferences.h"
#include "Operations/CortexBPDependentScan.h"
#include "Operations/CortexBPSaveGameGuard.h"

class FJsonObject;
class FProperty;
class UBlueprint;
struct FCortexCommandResult;

/**
 * blueprint.adopt_native_member: hands Blueprint variables over to properties the native parent already declares.
 *
 * The caller has already given the Blueprint a native parent that declares the member (reparent is a separate command).
 * The command retargets every node that names the variable (K2 get/set nodes, AnimGraph bindings, property access) while the
 * variable still exists, removes the variable declaration without destroying a node, compiles once, then writes the old
 * default into the native property on the CDO, loaded instances and loaded child Blueprint CDOs, and reads everything back.
 *
 * Params: asset_path, mappings:[{bp_variable, native_property, default?}], transfer_default?=true, retarget_nodes?=true,
 * dry_run?, expected_fingerprint?, compile?=true, save?=false, acknowledged_losses? (`save_game:<variable>`,
 * `instance_values:<variable>` and `default_unverified:<variable>` keys).
 */
class FCortexBPAdoptMemberOps
{
public:
	/** One requested hand-over. */
	struct FMappingRequest
	{
		FString BpVariable;
		FString NativeProperty;
		/** engine_already_adopted only: the value to write when the old default is gone. */
		bool bHasDefault = false;
		FString DefaultText;
	};

	/** A value captured for restoration and comparison. */
	struct FValueSnap
	{
		FString Text;
		bool bNumeric = false;
		double Number = 0.0;
		bool bValid = false;
	};

	/** A loaded object that carries a value of the member: a placed instance or a child Blueprint CDO. */
	struct FLoadedValue
	{
		FString Path;
		/** instance | child_cdo */
		FString Kind;
		FValueSnap Value;
		/** Renamed mappings: the native property's value on this object before the adoption (restored by a rollback). */
		FValueSnap NativeBefore;
		/** The value differs from the pre-adoption CDO value. */
		bool bIsOverride = false;
	};

	struct FMappingPlan
	{
		FString BpVariable;
		FString NativeProperty;
		/** variable_present | engine_already_adopted */
		FString State;
		/** exact | numeric_convertible | verified (engine_already_adopted) */
		FString TypeMatch;
		FString OldDefaultText;
		/** Renamed mappings: the native property's value on the generated CDO before the adoption (restored by a rollback). */
		FValueSnap NativeCdoBefore;
		/** compiled_cdo | variable_default | current_cdo_unverified | default_param | none */
		FString OldDefaultSource;
		/**
		 * engine_already_adopted without `default` while transfer_default: the engine conformed the variable before this call, so the
		 * original default is gone and OldDefault only mirrors the current native value. Requires `default_unverified:<variable>`.
		 */
		bool bDefaultUnverified = false;
		FValueSnap OldDefault;
		/** Every node that names the variable. */
		TArray<FCortexMemberReference> Nodes;
		/** Nodes the engine retargets together with the member. */
		TSet<FGuid> FollowNodes;
		TArray<FCortexBPSaveGameGuard::FFinding> SaveGameFindings;
		TArray<FLoadedValue> LoadedValues;
		TArray<FCortexBPDependentHit> Dependents;
		TArray<FString> Warnings;
		/** bp_variable != native_property. */
		bool bRename = false;
		bool bRedirectRequired = false;
		bool bRedirectPresent = false;
		FString RedirectLine;
		/** instance_values:<variable> was acknowledged: persisted instance values in unloaded maps are given up. */
		bool bInstanceValuesAcknowledged = false;
		/** The Blueprint variable and the native property are a double/float pair. */
		bool bNumericPair = false;

		/** Readback results per node GUID, filled by the apply step. */
		TMap<FGuid, bool> RetargetedNodes;
		TMap<FGuid, bool> ResolvedNodes;

		TSharedRef<FJsonObject> ToJson() const;
	};

	/** A reason the adoption cannot run, with the error code the refusal answers with. */
	struct FRefusal
	{
		FString Code;
		FString Message;
		TSharedPtr<FJsonObject> Details;
	};

	struct FAdoptPlan
	{
		TArray<FMappingPlan> Mappings;
		/** Sorted: save_game:<variable>. */
		TArray<FString> RequiredAcknowledgment;
		TArray<FRefusal> Refusals;
		bool bTransferDefault = true;
		bool bRetargetNodes = true;

		/** {mappings, required_acknowledgment, transfer_default, retarget_nodes, refusals}. Fields are added to Target. */
		void AppendTo(const TSharedRef<FJsonObject>& Target) const;
	};

	/**
	 * Read-only: never mutates the Blueprint. Acknowledged is the caller's sorted acknowledged_losses (only used to
	 * mark instance_values acknowledgments).
	 */
	static FAdoptPlan BuildPlan(
		UBlueprint* Blueprint,
		const TArray<FMappingRequest>& Mappings,
		bool bTransferDefault,
		bool bRetargetNodes,
		const TArray<FString>& Acknowledged);

	static FCortexCommandResult Execute(const TSharedPtr<FJsonObject>& Params);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook: runs inside the apply step after the references were retargeted and the variables removed (fault injection). */
	static void SetPostApplyHookForTest(TFunction<void(UBlueprint*)> Hook);
	static void ClearPostApplyHookForTest();

	/** Test hook: runs inside the readback after the default and instance values were written (fault injection). */
	static void SetPostWriteHookForTest(TFunction<void(UBlueprint*)> Hook);
	static void ClearPostWriteHookForTest();
#endif
};
