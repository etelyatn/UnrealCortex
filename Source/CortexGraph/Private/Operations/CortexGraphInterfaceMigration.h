#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexGraphMigrationOps.h"
#include "UObject/StrongObjectPtr.h"

class UBlueprint;
class UClass;
class UEdGraph;
class UFunction;
class FJsonObject;

/**
 * One mapped function of a `replace_interface` request: the implementation of an old interface function that moves to a
 * function of the new interface. Every field is a durable name or GUID, so apply, readback and recovery rebuild every
 * pointer.
 */
struct FCortexGraphInterfaceFunctionPlan
{
	FString From;
	FString To;
	/** function_graph | event */
	FString Implementation;
	/** True when the new function is event-style (placed as an event node), false when it is function-style (a graph). */
	bool bToIsEvent = false;
	bool bToHasResult = false;
	/** The old function graph (function_graph) or the event graph that owns the stale event (event). */
	FString SourceGraphGuid;
	FString SourceGraphName;
	/** Stale FunctionEntry or event node. */
	FString SourceEntryGuid;
	/** Stale FunctionResult node (function_graph with an output); empty otherwise. */
	FString SourceResultGuid;
	int32 NodeCount = 0;
	/** Deterministic identities of the replacement terminators (per function: entry:<to> / result:<to>). */
	FString EntryIdentity;
	FString ResultIdentity;
	/** The reviewed pin_map, canonical request entries {entry, from_pin, to_pin}. */
	TArray<TSharedPtr<FJsonValue>> PinMap;

	TSharedPtr<FJsonObject> ToJson() const;
	static bool FromJson(const TSharedPtr<FJsonObject>& Source, FCortexGraphInterfaceFunctionPlan& OutPlan, FCortexCommandResult& OutError);
};

/** An old interface function without an implementation that is dropped together with the old interface. */
struct FCortexGraphInterfaceDroppedStub
{
	FString Function;
	/** function_graph | event */
	FString Kind;
	FString GraphGuid;
	FString GraphName;
	/** Event node of an event stub. */
	FString NodeGuid;
};

/**
 * Durable, JSON-serializable plan of one `replace_interface` migration: the interface swap of one Blueprint. It carries only
 * names, GUIDs and canonical captures (the same contract as `FCortexGraphMigrationCallOutputPlan`).
 */
struct FCortexGraphMigrationInterfacePlan
{
	FString Op;
	/** Authoritative class paths of both interfaces. */
	FString OldInterface;
	FString NewInterface;
	TArray<FCortexGraphInterfaceFunctionPlan> Functions;
	TArray<FCortexGraphInterfaceDroppedStub> DroppedStubs;
	/** New function-style functions nothing maps onto: they receive an empty stub graph, as the engine creates it. */
	TArray<FString> NewStubFunctions;
	/** Preservation contract of every graph an entry replacement touches (all replaced nodes excluded). */
	TArray<FCortexGraphTransferPreservation> Preservations;
	int32 ScannedNodes = 0;
	/** True when the swap is already applied exactly: an idempotent replay that changes nothing. */
	bool bReused = false;

	TSharedPtr<FJsonObject> ToJson() const;
	static bool FromJson(const TSharedPtr<FJsonObject>& Source, FCortexGraphMigrationInterfacePlan& OutPlan, FCortexCommandResult& OutError);
};

/**
 * Exact pre-state of the interface list of one Blueprint: the interface classes in order, the graph membership of each
 * description and the name, GUID and InterfaceGuid of every graph, plus the graphs the swap created. The journal of a
 * `replace_interface` apply owns one; restoring and verifying it is how a failed apply returns to its exact pre-state.
 */
struct FCortexGraphInterfaceStateCapture
{
	struct FGraphState
	{
		TStrongObjectPtr<UEdGraph> Graph;
		FName Name = NAME_None;
		FGuid GraphGuid;
		FGuid InterfaceGuid;
	};
	struct FDescriptionState
	{
		TWeakObjectPtr<UClass> Interface;
		TArray<FGraphState> Graphs;
	};

	bool bCaptured = false;
	TArray<FDescriptionState> Descriptions;
	/** Graphs the swap created (new stubs); retired into the transient package on restore. */
	TArray<TStrongObjectPtr<UEdGraph>> AddedGraphs;

	void Capture(UBlueprint* Blueprint);
	/** Returns the interface list, the graph membership, the graph names and the InterfaceGuids to the captured state. */
	bool Restore(UBlueprint* Blueprint, FString& OutFailure);
	/** Exact comparison of the live interface state against the capture. */
	bool Matches(const UBlueprint* Blueprint, FString& OutFailure) const;
};

/** The `replace_interface` migration: planning, interface swap, readback and the preview inventory. */
class FCortexGraphInterfaceMigration
{
public:
	/** Resolves a class identifier (Blueprint asset path, class path or native class path) to its authoritative class; any class kind. */
	static bool ResolveClassIdentifier(const FString& Identifier, UClass*& OutClass, FCortexCommandResult& OutError);

	/** Resolves an interface identifier (BPI asset path, class path or native class path) to its authoritative class. */
	static bool ResolveInterfaceClass(const FString& Identifier, UClass*& OutClass, FCortexCommandResult& OutError);

	/** The authoritative class path of an interface. */
	static FString ClassPath(const UClass* Interface);

	/**
	 * Validates and normalizes one `replace_interface` migration without touching the asset: both interfaces, every
	 * implemented old function mapped, every `to` a function of the new interface, fail-closed pin_map coverage,
	 * name collisions, in-asset callers of the old interface and derived Blueprints that override a mapped function.
	 * A complete replay of an applied request sets bOutReused.
	 */
	static bool PlanInterfaceReplacement(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Migration,
		const FString& PatchId,
		FCortexGraphMigrationInterfacePlan& OutPlan,
		bool& bOutReused,
		FCortexCommandResult& OutError);

	/** The `replace_entry` migration object and implementation selector of one mapped function. */
	static void BuildEntryMigration(
		const FCortexGraphInterfaceFunctionPlan& Function,
		const FCortexGraphMigrationInterfacePlan& Plan,
		TSharedPtr<FJsonObject>& OutMigration,
		TSharedPtr<FJsonObject>& OutSelector);

	/**
	 * Interface swap: detaches the dropped stub graphs, adopts the graphs of the mapped function-style functions into a new
	 * description of the new interface (renamed when `to` differs, InterfaceGuid set), creates the stub graphs of the
	 * remaining new function-style functions and adds the new description. The old description stays (without graphs)
	 * until RemoveOldInterface. Every change is recorded in Capture so a failed apply restores the exact pre-state.
	 */
	static bool SwapInterfaces(
		UBlueprint* Blueprint,
		const FCortexGraphMigrationInterfacePlan& Plan,
		UClass* OldInterface,
		UClass* NewInterface,
		FCortexGraphInterfaceStateCapture& Capture,
		FString& OutFailure);

	/** Removes the (now graph-free) old interface description. */
	static bool RemoveOldInterface(UBlueprint* Blueprint, UClass* OldInterface, FString& OutFailure);

	/**
	 * Native readback of the swap: new interface present exactly once, old absent, every adopted graph owned by the new
	 * description with the new InterfaceGuid and function name, the new description holding exactly the function-style
	 * graphs, no node left that references the old interface, every entry replacement and every preservation contract.
	 * FaultProbe is the test-only readback divergence seam.
	 */
	static bool VerifyAgainstNative(
		UBlueprint* Blueprint,
		const FCortexGraphMigrationInterfacePlan& Plan,
		UClass* OldInterface,
		UClass* NewInterface,
		const TArray<FCortexGraphMigrationPlan>& EntryPlans,
		bool bCompiled,
		TFunctionRef<bool(const TCHAR*)> FaultProbe,
		FString& OutFailure);

	/** The compact preview inventory of a durable plan (null for an absent plan). */
	static TSharedPtr<FJsonObject> MakeInventory(const TSharedPtr<FJsonObject>& PlanJson);
};
