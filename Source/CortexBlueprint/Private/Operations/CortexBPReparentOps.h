#pragma once

#include "CoreMinimal.h"
#include "Operations/CortexBPSaveGameGuard.h"

class FJsonObject;
class UBlueprint;
class UClass;
struct FCortexCommandResult;

/**
 * blueprint.reparent: guarded parent change with a collision preflight, the editor's reparent compile flags,
 * a CDO delta and an explicit save.
 */
class FCortexBPReparentOps
{
public:
	/** One name clash between the Blueprint and the candidate parent. */
	struct FCollision
	{
		/** variable | function | scs */
		FString Kind;
		FString Name;
		/**
		 * variable: engine_replaces | adoptable_convertible | engine_renames | unverifiable;
		 * function: blocking; scs: adoptable | blocking.
		 */
		FString Resolution;
		/** variable only: the Blueprint and native types are the same type. */
		bool bTypeExact = false;
		bool bBlocking = false;
		FString Detail;

		/** `<kind>:<name>` */
		FString Key() const;
		/** True for the variables the editor/compiler removes from the Blueprint in favour of the native member. */
		bool ReplacesVariable() const;
		TSharedRef<FJsonObject> ToJson() const;
	};

	struct FReparentPlan
	{
		TArray<FCollision> Collisions;
		TArray<FCortexBPSaveGameGuard::FFinding> SaveGameFindings;
		/** Interfaces the Blueprint implements that the new parent already implements (reported only). */
		TArray<FString> InterfaceOverlap;
		/** Sorted: variable:<n> (replaced), scs:<n> (adoptable), save_game:<n>. */
		TArray<FString> RequiredAcknowledgment;
		/** Sorted keys of the collisions that refuse the reparent. */
		TArray<FString> Blocking;
		bool bHasScsCollision = false;

		/** {collisions, save_game_findings, interface_overlap, required_acknowledgment, blocking}. Fields are added to Target. */
		void AppendTo(const TSharedRef<FJsonObject>& Target) const;
	};

	/** Read-only. Never mutates the Blueprint. */
	static FReparentPlan BuildPlan(UBlueprint* Blueprint, UClass* NewParent);

	/** Params: asset_path, new_parent, dry_run?, expected_fingerprint?, compile?=true, save?=false, acknowledged_collisions?, report_path?. */
	static FCortexCommandResult Execute(const TSharedPtr<FJsonObject>& Params);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook: runs inside the apply step after the parent changed (fault injection). */
	static void SetPostApplyHookForTest(TFunction<void(UBlueprint*)> Hook);
	static void ClearPostApplyHookForTest();
#endif
};
