#pragma once

#include "CoreMinimal.h"
#include "CortexSTCompat.h"
#include "CortexSTNodeLocator.h"
#include "CortexSTTypes.h"
#include "CortexTypes.h"

class FJsonObject;
class FProperty;
class UClass;
class UScriptStruct;
class UStruct;

/** How a property of the old Blueprint instance is treated by the replacement. */
enum class ECortexSTReplacePropertyStatus : uint8
{
	/** The value is copied to the counterpart on the new type. */
	Copied,
	/** A node-level flag that still has its Blueprint default: nothing to carry over. */
	DefaultEqual,
	/** No counterpart, and the value is the class default: nothing is lost. */
	DroppedDefault,
	/** No counterpart and a non-default value: the call refuses until the key is in acknowledged_losses. */
	UnmappedNonDefault,
	/** Same as UnmappedNonDefault, but acknowledged: the value is dropped on purpose. */
	DroppedAcknowledged,
};

enum class ECortexSTReplacePropertyKind : uint8
{
	/** Instance-editable data of the Blueprint class (or of a native instance class). */
	Instance,
	/** A flag declared by the engine's Blueprint base class (bConsideredForCompletion, ...), mapped onto the node struct. */
	NodeFlag,
};

struct FCortexSTReplaceProperty
{
	FName OldName;
	/** NAME_None when there is no counterpart. */
	FName NewName;
	ECortexSTReplacePropertyKind Kind = ECortexSTReplacePropertyKind::Instance;
	ECortexSTReplacePropertyStatus Status = ECortexSTReplacePropertyStatus::DroppedDefault;
	bool bNonDefault = false;
	FString OldValue;
	const FProperty* OldProperty = nullptr;
	const FProperty* NewProperty = nullptr;
};

/** One side of one stored binding that the replacement rewrites (first segment: name, display name, GUID). */
struct FCortexSTReplaceRewrite
{
	int32 BindingIndex = INDEX_NONE;
	CortexSTCompat::EBindingSide Side = CortexSTCompat::EBindingSide::Target;
	/** True when the path is relative to the node struct (GetNodeID) instead of the instance data (ID). */
	bool bNodeStructSide = false;
	FString OldPath;
	FString NewPath;
	FGuid OldPropertyGuid;
	FGuid NewPropertyGuid;
	FName NewFirstSegment;
	bool bRenamed = false;
};

/** A binding that the replacement (or the validation that follows it) would drop or leave broken. */
struct FCortexSTReplaceUnresolved
{
	int32 BindingIndex = INDEX_NONE;
	CortexSTCompat::EBindingSide Side = CortexSTCompat::EBindingSide::Target;
	FString Path;
	FString Reason;
};

struct FCortexSTReplaceMismatch
{
	FName OldName;
	FName NewName;
	FString Reason;
};

/** The plan for one node. */
struct FCortexSTNodeReplacePlan
{
	FGuid NodeId;
	ECortexSTNodeSlot Slot = ECortexSTNodeSlot::Task;
	FString NodeName;
	FString OwnerStatePath;
	FString OldTypePath;
	TArray<FCortexSTReplaceProperty> Properties;
	TArray<FString> BlueprintOnlyMembers;
	TArray<FString> RequiredAcknowledgment;
	TArray<FCortexSTReplaceRewrite> Rewrites;
	TArray<FCortexSTReplaceUnresolved> Unresolved;
	TArray<FCortexSTReplaceMismatch> Mismatches;
	bool bWouldChange = true;
};

/** What the nodes become: exactly one of NodeStruct (native node struct) or NodeClass (native Blueprint-base class, wrapper kept). */
struct FCortexSTReplaceTarget
{
	const UScriptStruct* NodeStruct = nullptr;
	const UClass* NodeClass = nullptr;
};

struct FCortexSTReplaceRequest
{
	FCortexSTReplaceTarget Target;
	/** "OldProperty" -> "NewProperty". */
	TMap<FName, FName> PropertyMap;
	/** Keys of required_acknowledgment (old property names). */
	TSet<FString> AcknowledgedLosses;
};

struct FCortexSTReplacePlan
{
	TArray<FCortexSTNodeReplacePlan> Nodes;
	/** Whole-tree stored binding count before the call. */
	int32 BindingsBefore = 0;

	bool HasMismatches() const;
	bool HasUnresolved() const;
	bool NeedsAcknowledgment() const;
	bool WouldChange() const;
};

/**
 * statetree.replace_node: replaces Blueprint StateTree nodes by native ones in place, keeping the editor node ID so every binding keyed
 * by it survives. The data and the bindings move; EnterState/Tick/TestCondition logic is never ported.
 *
 * Params: asset_path, expected_fingerprint (required), exactly one of node_id | blueprint_class, exactly one of new_node_struct |
 * new_node_class, property_map?, acknowledged_losses?, dry_run? (default false), compile? (default true), save? (default false).
 *
 * Planning is pure: it classifies every property and every binding of the node before anything is mutated; a binding that would not
 * resolve on the new type is refused (VERIFICATION_FAILED), never dropped.
 */
class FCortexSTReplaceNodeOps
{
public:
	static FCortexCommandResult ReplaceNode(const TSharedPtr<FJsonObject>& Params);

	/**
	 * The pure plan builder. Returns false (OutError set) for a malformed request: wrong slot kind, unknown or ambiguous property_map
	 * entries. Soft findings (type mismatches, unacknowledged losses, unresolved bindings) are recorded in the plan.
	 */
	static bool BuildPlan(
		const FCortexSTAssetContext& Context,
		const TArray<FCortexSTNodeLocation>& Locations,
		const FCortexSTReplaceRequest& Request,
		FCortexSTReplacePlan& OutPlan,
		FCortexCommandResult& OutError);

	/** Where fault injection can intervene (test-only; the same pattern as SetApplyFaultPointForTesting in CortexGraphPatchOps). */
	enum class EFaultPoint : uint8
	{
		/** In place of the compile: true simulates a failed compile after the apply. */
		Compile,
		/** After the compile, before the readback verdict: true simulates a readback mismatch. */
		Readback,
		/** Inside the rollback: the journal restore is skipped (proves Cancel() alone restores nothing). */
		SkipJournalRestore,
		/** After the journal restore, before its verification: true marks the restore step as failed. */
		AfterRestore,
	};

#if WITH_AUTOMATION_TESTS
	static void SetFaultPointForTesting(EFaultPoint Point);
	static void ClearFaultPointForTesting();
#endif
};
