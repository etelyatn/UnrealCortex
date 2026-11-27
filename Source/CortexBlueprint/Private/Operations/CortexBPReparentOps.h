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

	/**
	 * A graph node that calls through a member (a Blueprint variable or a widget binding) whose type becomes a base type under the new
	 * parent: the native parent declares a same-named property of a base class of the Blueprint member's class, so the node's target
	 * is retyped when the compile adopts the native member.
	 */
	struct FTypeChangingCallSite
	{
		FString Member;
		/** The Blueprint member's class and the native base class it becomes. */
		FString OldType;
		FString NewType;
		FString Graph;
		FString NodeId;
		FString Node;
		/** The class the consuming pin requires (empty when unknown). */
		FString RequiredType;
		/** False when the new type is not a (sub)class of RequiredType: the connection breaks and the compile reports an error. */
		bool bResolvesOnNewType = true;

		TSharedRef<FJsonObject> ToJson() const;
	};

	struct FReparentPlan
	{
		TArray<FCollision> Collisions;
		TArray<FCortexBPSaveGameGuard::FFinding> SaveGameFindings;
		/** Interfaces the Blueprint implements that the new parent already implements (reported only). */
		TArray<FString> InterfaceOverlap;
		/** Non-blocking: nodes whose target member changes to a base type. */
		TArray<FTypeChangingCallSite> TypeChangingCallSites;
		/** Sorted: variable:<n> (replaced), scs:<n> (adoptable), save_game:<n>. */
		TArray<FString> RequiredAcknowledgment;
		/** Sorted keys of the collisions that refuse the reparent. */
		TArray<FString> Blocking;
		bool bHasScsCollision = false;

		/** {collisions, save_game_findings, interface_overlap, type_changing_call_sites, type_changing_call_site_count, warnings?, required_acknowledgment, blocking}. Fields are added to Target. */
		void AppendTo(const TSharedRef<FJsonObject>& Target) const;
	};

	/** Read-only. Never mutates the Blueprint. */
	static FReparentPlan BuildPlan(UBlueprint* Blueprint, UClass* NewParent);

	/** Params: asset_path, new_parent, dry_run?, expected_fingerprint?, compile?=true, save?=false, acknowledged_collisions?, report_path?, resync_embedded?=false. */
	static FCortexCommandResult Execute(const TSharedPtr<FJsonObject>& Params);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook: runs inside the apply step after the parent changed (fault injection). */
	static void SetPostApplyHookForTest(TFunction<void(UBlueprint*)> Hook);
	static void ClearPostApplyHookForTest();

	/** Test hook: runs after a successful guarded run, before the embedded templates are reported (simulates a template left behind by the compile). */
	static void SetPostRunHookForTest(TFunction<void(UBlueprint*)> Hook);
	static void ClearPostRunHookForTest();
#endif
};
