#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexGraphMigrationOps.h"

class UBlueprint;
class UClass;
class UEdGraph;
class UEdGraphNode;
class UFunction;
class UK2Node_CallFunction;
class FJsonObject;

/** One link endpoint of a reviewed pin: the far node and its pin. The far pin's direction is the opposite of the reviewed pin's. */
struct FCortexGraphRetargetLink
{
	FString NodeGuid;
	FString Pin;
};

/** One pin of a reviewed site: its identity, its canonical signature, the replacement pin it maps to and its links. */
struct FCortexGraphRetargetPin
{
	FString Name;
	/** Name of the split pin this pin is a child of (children are named `<parent>_<member>`); empty for a top-level pin. */
	FString Parent;
	/** EEdGraphPinDirection value (0 input, 1 output). */
	int32 Direction = 0;
	FString Signature;
	/** Name of the pin on the replacement node this pin maps to (same direction); empty when it has no counterpart. */
	FString MapsTo;
	TArray<FCortexGraphRetargetLink> Links;
};

/**
 * One call site of the retargeted function: a message node, a typed call or a cast to the old interface, with the per-pin
 * inventory the caller echoes back to approve exactly this set.
 */
struct FCortexGraphRetargetSite
{
	FString GraphGuid;
	FString SubgraphPath;
	FString NodeGuid;
	FString NodeClassPath;
	/** message | typed_call | interface_cast */
	FString Kind;
	TArray<FCortexGraphRetargetPin> Pins;
	/** Why this site cannot be retargeted (apply refuses a plan with any); empty when it can. */
	FString Blocked;
	/** Node a message->call site is replaced by (derived from the patch id); empty when the node keeps its identity. */
	FString ReplacementGuid;

	/** The identity the canonical comparison of an echoed site uses: every published field, pins and links sorted. */
	FString Key() const;
	TSharedPtr<FJsonObject> ToJson() const;
	static bool FromJson(const TSharedPtr<FJsonObject>& Source, FCortexGraphRetargetSite& OutSite, FCortexCommandResult& OutError);
};

struct FCortexGraphRetargetPinMapping
{
	FString FromPin;
	FString ToPin;
};

/** A graph that owns at least one site: its pre-state node set (as the expected post-state digest) and node count. */
struct FCortexGraphRetargetGraph
{
	FString GraphGuid;
	FString SubgraphPath;
	int32 NodeCount = 0;
	/** Digest of the node GUID set the graph must hold after the apply: pre-state minus replaced message nodes plus their replacements. */
	FString ExpectedNodeDigest;
};

/**
 * Durable, JSON-serializable plan of one `retarget_calls` migration: the endpoints, the pin map, the reviewed site set and the
 * preservation contract of every touched graph. It carries only names, GUIDs and canonical captures.
 */
struct FCortexGraphMigrationRetargetPlan
{
	FString Op;
	FString FromOwner;
	FString FromFunction;
	FString ToOwner;
	FString ToFunction;
	/** message | call */
	FString Mode;
	TArray<FCortexGraphRetargetPinMapping> PinMap;
	TArray<FCortexGraphRetargetSite> Sites;
	TArray<FCortexGraphRetargetGraph> Graphs;
	TArray<FCortexGraphTransferPreservation> Preservations;
	int32 ScannedNodes = 0;
	/** True when the request carried no `sites`: the plan publishes the discovered set for review (never part of the validation hash). */
	bool bAwaitingApproval = false;

	bool HasBlockedSite() const;
	TSharedPtr<FJsonObject> ToJson() const;
	static bool FromJson(const TSharedPtr<FJsonObject>& Source, FCortexGraphMigrationRetargetPlan& OutPlan, FCortexCommandResult& OutError);
};

/** The `retarget_calls` migration: planning, node mutation primitives, readback and the preview inventory. */
class FCortexGraphRetargetOps
{
public:
	static constexpr int32 MaxSites = 512;

	/**
	 * Validates and normalizes one `retarget_calls` migration without touching the asset: both endpoints, the pin map, the
	 * complete site set of the asset (messages, typed calls and casts of the old owner, matched by resolved owner), the per-site
	 * link inventory, the per-link connectability of the replacement and the exact match of the caller's echoed `sites`.
	 */
	static bool PlanRetarget(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Migration,
		const FString& PatchId,
		FCortexGraphMigrationRetargetPlan& OutPlan,
		FCortexCommandResult& OutError);

	/** Re-resolves the classes and the target function of a plan. */
	static bool ResolveEndpoints(
		const FCortexGraphMigrationRetargetPlan& Plan,
		UClass*& OutFromOwner,
		UClass*& OutToOwner,
		UFunction*& OutToFunction,
		FCortexCommandResult& OutError);

	/**
	 * Classifies one node against the retargeted endpoint: message, typed_call or interface_cast, matched by resolved owner and
	 * never by name alone. A node of a call or cast subclass this op does not rewrite is reported with its own kind and
	 * `bOutUnsupportedClass` so the site is published as blocked instead of silently skipped.
	 */
	static bool ClassifySite(
		const UEdGraphNode* Node,
		const UClass* FromOwner,
		const FString& FromFunction,
		FString& OutKind,
		bool& bOutUnsupportedClass);

	/** Every remaining site of the old endpoint in the asset (the readback proves there is none). */
	static int32 CountRemainingSites(
		UBlueprint* Blueprint,
		const UClass* FromOwner,
		const FString& FromFunction,
		FString& OutFirstSite);

	/**
	 * Points a message or typed-call node at the new function (`FunctionReference.SetExternalMember`) or a cast at the new
	 * class and reconstructs it. The caller has broken every link of the node first, so the reconstruction neither orphans nor
	 * re-links anything; the reviewed links are re-made afterwards.
	 */
	static bool ApplyNodeRetarget(
		UEdGraphNode* Node,
		const FCortexGraphRetargetSite& Site,
		UClass* ToOwner,
		const FString& ToFunction,
		FString& OutFailure);

	/**
	 * Creates the direct typed call that replaces a message node, at the planned identity, with the old node's editor state and
	 * the literals of its mapped, unlinked input pins.
	 */
	static UK2Node_CallFunction* CreateCallReplacement(
		UEdGraph* Graph,
		const UEdGraphNode* Old,
		const FCortexGraphRetargetSite& Site,
		const FGuid& ReplacementGuid,
		UClass* ToOwner,
		const FString& ToFunction,
		FString& OutFailure);

	/**
	 * Native readback: the exact node set of every touched graph, every site's replacement node (class, function reference,
	 * resolved target), its links by (node GUID, pin name) against the reviewed inventory mapped through the pins' `maps_to`,
	 * the replaced message nodes gone, no remaining site of the old endpoint and the preservation contracts.
	 * FaultProbe is the test-only readback divergence seam.
	 */
	static bool VerifyRetargetAgainstNative(
		UBlueprint* Blueprint,
		const FCortexGraphMigrationRetargetPlan& Plan,
		TFunctionRef<bool(const TCHAR*)> FaultProbe,
		FString& OutFailure);

	/** The compact preview/apply inventory of a durable plan (null for an absent plan). */
	static TSharedPtr<FJsonObject> MakeInventory(const TSharedPtr<FJsonObject>& PlanJson);
	static TSharedPtr<FJsonObject> MakeInventory(const FCortexGraphMigrationRetargetPlan& Plan);

	/** Canonical identity of one link endpoint, used by the apply, the journal and the readback alike. */
	static FString LinkKey(const FString& NodeGuid, const FString& PinName, int32 Direction);
};
