#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class FJsonObject;

/**
 * Deterministic identities of the replacement terminators of one `replace_entry` request.
 *
 * Both GUIDs are derived from the patch id by the same derivation the authoring shell uses for
 * client ids, so a repeated identical request reconciles through the existing deterministic
 * identity scheme instead of inventing a second one.
 */
struct FCortexGraphMigrationIdentity
{
	FGuid EntryGuid;
	FGuid ResultGuid;
};

/**
 * One validated pin_map entry: the stale pin of the source terminator and the replacement pin it
 * maps onto. `TargetRole` is the replacement terminator that owns `ToPin`; `Entry` is the declared
 * direction (`input` / `output`), which must equal the replacement pin's real direction.
 */
struct FCortexGraphMigrationPinMapping
{
	FString Entry;
	FString TargetRole;
	FString FromPin;
	FString ToPin;
	int32 FromDirection = 0;
};

/**
 * One link of the replacement terminators, canonicalized so readback, apply and recovery agree.
 *
 * Endpoint A is always a pin of a replacement terminator. Endpoint B is either a preserved node
 * identified by GUID (`FarRole` empty) or the sibling replacement terminator (`FarRole` set), which
 * is how the entry/result link inside the replaced set survives the replacement.
 */
struct FCortexGraphMigrationEdge
{
	FString ReplacementRole;
	FString ReplacementPin;
	FString FarRole;
	FString FarGuid;
	FString FarPin;
};

/** A link the apply created, reported back so the journal can reverse it by durable identity. */
struct FCortexGraphMigrationLink
{
	FGuid FromNodeGuid;
	FName FromPin;
	FGuid ToNodeGuid;
	FName ToPin;
};

/**
 * Durable, JSON-serializable plan of one `replace_entry` migration.
 *
 * It carries only names, GUIDs and canonical descriptors, so a prepared patch survives without
 * retaining UObjects and the apply phase can rebuild every transient pointer after the final guard.
 */
struct FCortexGraphMigrationPlan
{
	FString Op;
	FString GraphGuid;
	FString SubgraphPath;
	FString SourceEntryGuid;
	/** Empty when the replaced set has no result terminator (an event entry, or a function without out params). */
	FString SourceResultGuid;
	FCortexGraphMigrationIdentity Identity;
	bool bIsEvent = false;
	/** True when the target declaration owns a result terminator, so the replaced set is a pair. */
	bool bHasResultTerminator = false;
	FString FunctionName;
	FString OwnerClassPath;
	/** Canonical pin signature descriptors of the replacement entry terminator. */
	TArray<TSharedPtr<FJsonValue>> EntryPins;
	/** Canonical pin signature descriptors of the replacement result terminator (empty when absent). */
	TArray<TSharedPtr<FJsonValue>> ResultPins;
	TArray<FCortexGraphMigrationPinMapping> PinMap;
	TArray<FCortexGraphMigrationEdge> Edges;
	int32 NodePosX = 0;
	int32 NodePosY = 0;
	FString NodeComment;
	bool bCommentBubblePinned = false;
	bool bCommentBubbleVisible = false;
	int32 EnabledState = 0;
	bool bUserSetEnabledState = false;
	bool bForceDisplayAsDisabled = false;
	bool bRemoveMember = false;
	FString MemberName;
	FString MemberKind;
	FString MemberNodeGuid;
	/**
	 * In-asset nodes that reference the removed shadowing member. The member removal destroys them,
	 * so the apply detaches and journals them first and recovery restores them with the member.
	 */
	TArray<FString> MemberReferenceNodeGuids;
	/** Canonical capture of every node outside the replaced set, taken during preflight. */
	FString PreservationCapture;
	/** Reference inventory of the replaced declaration: resolved call sites and bindings, in-asset and across loaded packages. */
	int32 DeclarationReferences = 0;
	int32 ExternalDeclarationReferences = 0;
	/** References that name the declaration but cannot be resolved to a concrete owner; any one blocks. */
	TArray<FString> UnresolvedDeclarationReferences;
	/** Authoring-shaped descriptor of the replacement entry, so the shared readback also covers it. */
	TSharedPtr<FJsonObject> NormalizedNode;
	/** Resolved declaration symbol, shared with the authoring readback. */
	TSharedPtr<FJsonObject> ResolvedSymbol;

	/** True when the target declaration owns a result terminator, so the replaced set is a pair. */
	bool HasResultTerminator() const { return bHasResultTerminator; }

	TSharedPtr<FJsonObject> ToJson() const;
	static bool FromJson(
		const TSharedPtr<FJsonObject>& Source,
		FCortexGraphMigrationPlan& OutPlan,
		FCortexCommandResult& OutError);
};

class FCortexGraphMigrationOps
{
public:
	/**
	 * Validates and normalizes one `replace_entry` migration without touching the asset: source
	 * entry eligibility, reference inventory, shadowing-member policy, fail-closed pin_map coverage
	 * and the pin-level compatibility diff. On success OutPlan is complete and durable.
	 */
	static bool Plan(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Migration,
		const TSharedPtr<FJsonObject>& TargetSelector,
		const FCortexGraphMigrationIdentity& Identity,
		FCortexGraphMigrationPlan& OutPlan,
		bool& bOutReused,
		FCortexCommandResult& OutError);

	/**
	 * Native comparison of the planned replacement against the live asset: replacement terminator
	 * symbols, every mapped pin signature, the realized boundary links and, when asked, the
	 * downstream preservation capture. Shared by the idempotent-reuse reconciliation and readback.
	 */
	static bool VerifyReplacementAgainstNative(
		UBlueprint* Blueprint,
		const FCortexGraphMigrationPlan& Plan,
		bool bCompiled,
		bool bVerifyPreservation,
		FString& OutFailure);

	/** Registers the replacement terminators in the live graph with their deterministic identities. */
	static bool RegisterReplacement(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FCortexGraphMigrationPlan& Plan,
		UEdGraphNode*& OutEntry,
		UEdGraphNode*& OutResult,
		FCortexCommandResult& OutError);

	/**
	 * Realizes the planned boundary links on the replacement terminators through the graph schema.
	 * Links whose far endpoint is a preserved node keep that node and its pin untouched.
	 */
	static bool RemapBoundary(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FCortexGraphMigrationPlan& Plan,
		UEdGraphNode* Entry,
		UEdGraphNode* Result,
		TArray<FCortexGraphMigrationLink>& OutCreatedLinks,
		FCortexCommandResult& OutError);

	/** Canonical capture of every node outside ExcludedGuids plus their links inside that set. */
	static FString CapturePreservation(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TArray<FGuid>& ExcludedGuids);

	/** Removes one inventoried Blueprint variable. The caller journals the captured variable first. */
	static bool RemoveShadowingMember(
		UBlueprint* Blueprint,
		const FName MemberName,
		FCortexCommandResult& OutError);

	/** Re-inserts one captured Blueprint variable description exactly where it was. */
	static bool RestoreShadowingMember(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& CapturedVariable,
		int32 Index,
		FCortexCommandResult& OutError);

	/** Durable capture of one Blueprint variable: the complete description, not only its fingerprint fields. */
	static TSharedPtr<FJsonObject> CaptureShadowingMember(UBlueprint* Blueprint, const FName MemberName);

	/**
	 * Field-by-field comparison of a live member against its captured description. The authoring
	 * fingerprint cannot see rep-notify, replication, metadata or the remaining pin-type flags, so
	 * recovery has to prove member restoration here instead.
	 */
	static bool MemberMatchesCapture(
		UBlueprint* Blueprint,
		const FName MemberName,
		const TSharedPtr<FJsonObject>& Captured,
		FString& OutFailure);

	static UEdGraphNode* FindNodeByGuid(UBlueprint* Blueprint, const FGuid& NodeGuid);
	static UEdGraph* FindGraphByGuid(UBlueprint* Blueprint, const FGuid& GraphGuid);
};
