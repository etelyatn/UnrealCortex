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
 * One selected node of a bounded same-asset transfer.
 *
 * `SourceGuid` and `DestinationGuid` are the complete deterministic identity map; every capture is
 * presentation-shaped, so readback compares real authored state of both graphs without holding a
 * pointer across the request boundary.
 */
struct FCortexGraphTransferNode
{
	FString SourceGuid;
	FString DestinationGuid;
	FString ClassPath;
	/** Canonical symbol descriptor of the node's authored symbol (call target, variable, cast). */
	FString Symbol;
	int32 PosX = 0;
	int32 PosY = 0;
	FString Comment;
	bool bCommentBubblePinned = false;
	bool bCommentBubbleVisible = false;
	int32 EnabledState = 0;
	bool bUserSetEnabledState = false;
	bool bForceDisplayAsDisabled = false;
	/** Canonical capture of the node's pins, defaults and internal links (endpoints as slot indices). */
	FString Pins;
};

/** One link whose two endpoints are both inside the selected set; transferred automatically. */
struct FCortexGraphTransferEdge
{
	FString FromGuid;
	FString FromPin;
	FString ToGuid;
	FString ToPin;
};

/**
 * One explicit boundary mapping: a crossing edge of the selection and the destination endpoint that
 * replaces it. The source crossing edge is never preserved literally, so an uncovered edge can only
 * be refused, never silently dropped.
 */
struct FCortexGraphTransferBoundary
{
	FString SourceGuid;
	FString SourcePin;
	/** The far endpoint of the covered source crossing edge, reported so the refusal names the edge. */
	FString SourceFarGuid;
	FString SourceFarPin;
	FString DestinationNodeGuid;
	FString DestinationPin;
};

/** One inventoried dependency of the selection: preview output and refusal source. */
struct FCortexGraphTransferDependency
{
	FString NodeGuid;
	/** local_variable | interface | member | external */
	FString Kind;
	FString Member;
	FString OwnerClass;
	FString Type;
	FString Detail;
};

/** One graph preservation contract, verified by readback and by both-graph recovery. */
struct FCortexGraphTransferPreservation
{
	/** source_selection | source_graph | destination_graph */
	FString Label;
	FString GraphGuid;
	TArray<FString> ExcludedGuids;
	FString Capture;
};

/**
 * Durable, JSON-serializable plan of one bounded same-asset `copy_subgraph` / `move_subgraph`.
 *
 * Like the replacement plan it carries only names, GUIDs and canonical captures, so the apply phase
 * rebuilds every transient pointer after the final guard and the readback comparator compares
 * native state instead of the request.
 */
struct FCortexGraphMigrationTransferPlan
{
	/** copy_subgraph | move_subgraph */
	FString Op;
	FString SourceGraphGuid;
	FString SourceSubgraphPath;
	FString DestinationGraphGuid;
	FString DestinationSubgraphPath;
	TArray<FCortexGraphTransferNode> Nodes;
	TArray<FCortexGraphTransferEdge> InternalEdges;
	TArray<FCortexGraphTransferBoundary> Boundary;
	TArray<FCortexGraphTransferDependency> Dependencies;
	/** Source GUIDs a `move_subgraph` removes; empty for a copy. */
	TArray<FString> RemovalSet;
	TArray<FCortexGraphTransferPreservation> Preservations;
	/** True when the destination identity set already matched exactly: an idempotent replay. */
	bool bReused = false;

	bool IsMove() const { return Op == TEXT("move_subgraph"); }

	TSharedPtr<FJsonObject> ToJson() const;
	static bool FromJson(
		const TSharedPtr<FJsonObject>& Source,
		FCortexGraphMigrationTransferPlan& OutPlan,
		FCortexCommandResult& OutError);
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
	/** True when an accepted replay named a source locator that no longer exists (reported, never inferred). */
	bool bReplayedWithAbsentSource = false;
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

	/**
	 * Validates and normalizes one bounded same-asset `copy_subgraph` / `move_subgraph` without
	 * touching the asset: selection membership, supported node kinds, the complete crossing-edge
	 * inventory against the explicit boundary map, the dependency inventory, the asset-wide
	 * destination identity set and the idempotent-replay reconciliation. On success OutPlan is
	 * complete and durable.
	 */
	static bool PlanTransfer(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Migration,
		const FString& PatchId,
		FCortexGraphMigrationTransferPlan& OutPlan,
		bool& bOutReused,
		FCortexCommandResult& OutError);

	/**
	 * Creates the destination nodes of a planned transfer inside its destination graph through the
	 * engine node-clone path, keeps every planned authored field, and refuses a clone that is
	 * incomplete instead of accepting a silently substituted node set.
	 */
	static bool RegisterTransferNodes(
		UBlueprint* Blueprint,
		const FCortexGraphMigrationTransferPlan& Plan,
		TArray<FGuid>& OutCreatedGuids,
		FCortexCommandResult& OutError);

	/**
	 * Realizes the planned internal edges and every planned boundary mapping through the graph
	 * schema. Nothing is created by hand: the schema owns connection safety on both sides.
	 */
	static bool WireTransfer(
		UBlueprint* Blueprint,
		const FCortexGraphMigrationTransferPlan& Plan,
		TArray<FCortexGraphMigrationLink>& OutCreatedLinks,
		FCortexCommandResult& OutError);

	/**
	 * Native comparison of the planned transfer against the live asset: destination node classes,
	 * authored state through the canonical capture, internal edges, realized boundary links, the
	 * asset-wide identity set and, for a move, the absence of the moved nodes in the source graph.
	 * Shared by the readback and by the idempotent-reuse reconciliation.
	 */
	static bool VerifyTransferAgainstNative(
		UBlueprint* Blueprint,
		const FCortexGraphMigrationTransferPlan& Plan,
		FString& OutFailure);

	/** Compares graph preservation contracts against live native state. */
	static bool VerifyPreservationContracts(
		UBlueprint* Blueprint,
		const TArray<FCortexGraphTransferPreservation>& Contracts,
		FString& OutFailure);

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
	/** Graph-scoped lookup: the durable way to resolve a node identity inside one named graph. */
	static UEdGraphNode* FindNodeByGuidInGraph(UEdGraph* Graph, const FGuid& NodeGuid);
	static UEdGraph* FindGraphByGuid(UBlueprint* Blueprint, const FGuid& GraphGuid);
};
