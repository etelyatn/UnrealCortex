#include "CortexGraphCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Operations/CortexGraphConnectionOps.h"
#include "Operations/CortexGraphTraceOps.h"
#include "Operations/CortexGraphAuthoringContext.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "CortexAssetMutationGuard.h"
#include "Engine/Blueprint.h"

namespace
{
bool RejectBlockedGraphMutation(const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& OutError)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return false;
	FString Reason;
	if (FCortexAssetMutationGuard::IsPathBlocked(AssetPath, Reason))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *Reason));
		return true;
	}
	return false;
}

/** Canonical 32-character GUID string, so every published identity uses one representation. */
FString CanonicalGuidString(const FString& Value)
{
	FGuid Parsed;
	return FGuid::Parse(Value, Parsed) ? Parsed.ToString() : Value;
}

/** Deterministic client id -> node GUID mapping, ordered so two equal runs serialize equally. */
TSharedPtr<FJsonObject> MakeNodeMappingJson(const TMap<FString, FGuid>& NodeGuidByClientId)
{
	TSharedPtr<FJsonObject> Mappings = MakeShared<FJsonObject>();
	TArray<FString> ClientIds;
	NodeGuidByClientId.GetKeys(ClientIds);
	ClientIds.Sort();
	for (const FString& ClientId : ClientIds)
	{
		Mappings->SetStringField(ClientId, NodeGuidByClientId[ClientId].ToString());
	}
	return Mappings;
}

/** Durable locators of a preview or an applied patch; an unresolved identity stays absent. */
TSharedPtr<FJsonObject> MakeLocatorsJson(
	const FGuid& GraphGuid,
	const FString& SubgraphPath,
	const FGuid& EntryNodeGuid,
	const bool bHasEntryNode)
{
	TSharedPtr<FJsonObject> Locators = MakeShared<FJsonObject>();
	if (GraphGuid.IsValid())
	{
		Locators->SetStringField(TEXT("graph_guid"), GraphGuid.ToString());
	}
	if (!SubgraphPath.IsEmpty())
	{
		Locators->SetStringField(TEXT("subgraph_path"), SubgraphPath);
	}
	if (bHasEntryNode && EntryNodeGuid.IsValid())
	{
		Locators->SetStringField(TEXT("entry_node_guid"), EntryNodeGuid.ToString());
	}
	Locators->SetBoolField(TEXT("has_entry_node"), bHasEntryNode);
	return Locators;
}

/**
 * The phase part of the compact patch result: identity, phase statuses, counts, fingerprints, dirty
 * state and the bounded diagnostics. It deliberately carries no full graph data (that stays a
 * separate bounded read) so no response-size guard can turn a real mutation into an ambiguous
 * failure. A patch id the request never named, and live state the handler could not read, are
 * omitted instead of fabricated.
 */
TSharedPtr<FJsonObject> MakePatchPhaseJson(
	const FString& PatchId,
	const bool bChanged,
	const bool bDryRun,
	const FCortexGraphPatchOutcome& Outcome,
	const bool bIncludeLiveState)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	if (!PatchId.IsEmpty())
	{
		Data->SetStringField(TEXT("patch_id"), CanonicalGuidString(PatchId));
	}
	Data->SetBoolField(TEXT("changed"), bChanged);
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetStringField(TEXT("apply_status"), Outcome.ApplyStatus);
	Data->SetStringField(TEXT("compile_status"), Outcome.CompileStatus);
	Data->SetStringField(TEXT("readback_status"), Outcome.ReadbackStatus);
	Data->SetStringField(TEXT("rollback_status"), Outcome.RollbackStatus);
	Data->SetStringField(TEXT("save_status"), Outcome.SaveStatus);
	Data->SetStringField(TEXT("post_save_status"), Outcome.PostSaveStatus);
	Data->SetNumberField(TEXT("target_compile_count"), Outcome.TargetCompileCount);
	Data->SetNumberField(TEXT("recovery_compile_count"), Outcome.RecoveryCompileCount);
	Data->SetBoolField(TEXT("saved"), Outcome.bSaved);
	Data->SetBoolField(TEXT("blocked"), Outcome.bBlocked);
	Data->SetBoolField(TEXT("replayed_with_absent_source"), Outcome.bReplayedWithAbsentSource);
	if (bIncludeLiveState)
	{
		if (Outcome.FingerprintBefore.IsValid())
		{
			Data->SetObjectField(TEXT("fingerprint_before"), Outcome.FingerprintBefore);
		}
		if (Outcome.FingerprintAfter.IsValid())
		{
			Data->SetObjectField(TEXT("fingerprint_after"), Outcome.FingerprintAfter);
		}
		Data->SetBoolField(TEXT("dirty_before"), Outcome.bDirtyBefore);
		Data->SetBoolField(TEXT("dirty_after"), Outcome.bDirtyAfter);
	}

	TArray<TSharedPtr<FJsonValue>> Reused;
	for (const FString& ClientId : Outcome.ReusedClientIds)
	{
		Reused.Add(MakeShared<FJsonValueString>(ClientId));
	}
	Data->SetArrayField(TEXT("reused_client_ids"), Reused);

	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const FString& Diagnostic : Outcome.Diagnostics)
	{
		Diagnostics.Add(MakeShared<FJsonValueString>(Diagnostic));
	}
	Data->SetArrayField(TEXT("diagnostics"), Diagnostics);
	return Data;
}

/** The complete compact patch result: the phases plus every identity the patch planned or reused. */
TSharedPtr<FJsonObject> MakePatchResultJson(
	const FString& PatchId,
	const bool bChanged,
	const bool bDryRun,
	const FCortexGraphPatchOutcome& Outcome,
	const TMap<FString, FGuid>& NodeGuidByClientId,
	const FGuid& GraphGuid,
	const FString& SubgraphPath,
	const FGuid& EntryNodeGuid,
	const bool bHasEntryNode)
{
	TSharedPtr<FJsonObject> Data = MakePatchPhaseJson(PatchId, bChanged, bDryRun, Outcome, true);
	Data->SetObjectField(TEXT("node_mappings"), MakeNodeMappingJson(NodeGuidByClientId));
	Data->SetObjectField(TEXT("locators"), MakeLocatorsJson(GraphGuid, SubgraphPath, EntryNodeGuid, bHasEntryNode));
	return Data;
}

/** The patch id a request names, or empty when the field is absent or is not a GUID. */
FString RequestedPatchId(const TSharedPtr<FJsonObject>& Params)
{
	FString Raw;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("patch_id"), Raw))
	{
		return FString();
	}
	FGuid Parsed;
	return FGuid::Parse(Raw, Parsed) ? Parsed.ToString() : FString();
}

/** The requested dry_run flag, defaulting to the contract's preview default. */
bool RequestedDryRun(const TSharedPtr<FJsonObject>& Params)
{
	bool bDryRun = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	}
	return bDryRun;
}

/** Diagnostics of one refusal: the structured error the caller must act on, under the shared bound. */
TArray<FString> MakeRefusalDiagnostics(const FCortexCommandResult& Error)
{
	TArray<FString> Diagnostics;
	Diagnostics.Add(Error.ErrorMessage.IsEmpty()
		? Error.ErrorCode
		: FString::Printf(TEXT("%s: %s"), *Error.ErrorCode, *Error.ErrorMessage));
	FCortexGraphPatchOps::TrimDiagnostics(Diagnostics);
	return Diagnostics;
}

/**
 * A refusal that happened while the asset was loaded but before any patch work: the requested
 * identity, change-free phase statuses and the live before/after state. Nothing was planned, so no
 * client-id mapping and no locator is published.
 */
TSharedPtr<FJsonObject> MakeLoadedRefusalJson(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params,
	const FCortexCommandResult& Error)
{
	FCortexGraphPatchOutcome Outcome;
	Outcome.FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint);
	Outcome.FingerprintAfter = Outcome.FingerprintBefore;
	Outcome.bDirtyBefore = Blueprint->GetOutermost()->IsDirty();
	Outcome.bDirtyAfter = Outcome.bDirtyBefore;
	Outcome.Diagnostics = MakeRefusalDiagnostics(Error);
	return MakePatchPhaseJson(RequestedPatchId(Params), false, RequestedDryRun(Params), Outcome, true);
}

/**
 * A refusal that happened before the asset was loaded: no fingerprint, dirty state, mapping or
 * locator can be stated honestly, so only the requested identity, the statuses and the reason are.
 */
TSharedPtr<FJsonObject> MakeUnloadedRefusalJson(
	const TSharedPtr<FJsonObject>& Params,
	const FCortexCommandResult& Error)
{
	FCortexGraphPatchOutcome Outcome;
	Outcome.Diagnostics = MakeRefusalDiagnostics(Error);
	return MakePatchPhaseJson(RequestedPatchId(Params), false, RequestedDryRun(Params), Outcome, false);
}

/**
 * `graph.apply_patch` dispatches preview and apply through the frozen T06-T09 contract: preview is
 * a pure preflight, apply is the full coordinated coordinator, and every terminal path reports the
 * compact result — including a structured failure that still carries the applied state.
 */
FCortexCommandResult HandleApplyPatch(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		FCortexCommandResult MissingAssetPath =
			FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
		MissingAssetPath.ErrorDetails = MakeUnloadedRefusalJson(Params, MissingAssetPath);
		return MissingAssetPath;
	}

	// Entry-point selection only. The envelope validator re-reads the flag and owns its exact
	// error, so a malformed flag type is reported once, by the coordinator.
	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		LoadError.ErrorDetails = MakeUnloadedRefusalJson(Params, LoadError);
		return LoadError;
	}

	if (bDryRun)
	{
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult PreviewError;
		if (!FCortexGraphPatchOps::Preflight(Blueprint, Params, Prepared, PreviewError))
		{
			// The asset is loaded and the read is non-mutating, so a refused preview still reports
			// the requested identity, the change-free statuses and the live before/after state.
			PreviewError.ErrorDetails = MakeLoadedRefusalJson(Blueprint, Params, PreviewError);
			return PreviewError;
		}
		// A preview is an outcome whose phases never ran: patch identity, planned identities and
		// the live fingerprint are known, every phase status stays not_requested.
		FCortexGraphPatchOutcome Preview;
		Preview.PatchId = Prepared.PatchId;
		Preview.bChanged = Prepared.bChanged;
		Preview.FingerprintBefore = Prepared.FingerprintBefore;
		Preview.FingerprintAfter = Prepared.FingerprintBefore;
		Preview.bDirtyBefore = Blueprint->GetOutermost()->IsDirty();
		Preview.bDirtyAfter = Preview.bDirtyBefore;
		Preview.ReusedClientIds = Prepared.ReusedClientIds;
		// A caller must learn at preview time that the locator they named is not present, so the
		// preview carries the same provenance flag and diagnostic the apply reports.
		Preview.bReplayedWithAbsentSource = Prepared.bReplayedWithAbsentSource;
		if (Prepared.bReplayedWithAbsentSource)
		{
			Preview.Diagnostics.Add(TEXT(
				"replay accepted with an absent source locator: the migration apply already consumed the stale entry this request names"));
		}
		// The planned graph locator is knowledge the caller needs before applying.
		FGuid PreparedGraphGuid;
		FGuid::Parse(Prepared.GraphGuid, PreparedGraphGuid);
		TSharedPtr<FJsonObject> Data = MakePatchResultJson(
			Prepared.PatchId, Prepared.bChanged, true, Preview, Prepared.NodeGuidByClientId,
			PreparedGraphGuid, Prepared.SubgraphPath, Prepared.EntryNodeGuid, Prepared.bHasEntryNode);
		Data->SetStringField(TEXT("validation_hash"), Prepared.ValidationHash);
		return FCortexCommandRouter::Success(Data);
	}

	FCortexGraphPatchOutcome Outcome;
	FCortexCommandResult Error;
	if (!FCortexGraphPatchOps::Execute(Blueprint, Params, Outcome, Error))
	{
		// A failed apply still reports the compact result, so a verified in-memory outcome that
		// could not be persisted is never reported as an ambiguous failure. A refusal that happened
		// before the coordinator learned the identity still reports the patch id the request named,
		// never invents mappings for work that never ran, and always names the failure.
		const FString FailurePatchId = Outcome.PatchId.IsEmpty() ? RequestedPatchId(Params) : Outcome.PatchId;
		if (Outcome.Diagnostics.Num() == 0)
		{
			// No compiler or rollback diagnostics exist yet, so the refusal itself is the diagnostic.
			Outcome.Diagnostics = MakeRefusalDiagnostics(Error);
		}
		Error.ErrorDetails = MakePatchResultJson(
			FailurePatchId, Outcome.bChanged, false, Outcome, Outcome.Locators.NodeGuidByClientId,
			Outcome.Locators.GraphGuid, Outcome.Locators.SubgraphPath, Outcome.Locators.EntryNodeGuid,
			Outcome.Locators.bHasEntryNode);
		return Error;
	}

	return FCortexCommandRouter::Success(MakePatchResultJson(
		Outcome.PatchId, Outcome.bChanged, false, Outcome, Outcome.Locators.NodeGuidByClientId,
		Outcome.Locators.GraphGuid, Outcome.Locators.SubgraphPath, Outcome.Locators.EntryNodeGuid,
		Outcome.Locators.bHasEntryNode));
}
}

FCortexCommandResult FCortexGraphCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

	if (Command == TEXT("get_authoring_context") || Command == TEXT("authoring_context"))
	{
		return FCortexGraphAuthoringContext::Read(Params);
	}
	if (Command == TEXT("add_node") || Command == TEXT("remove_node") || Command == TEXT("connect")
		|| Command == TEXT("disconnect") || Command == TEXT("set_pin_value") || Command == TEXT("auto_layout")
		|| Command == TEXT("apply_patch"))
	{
		FCortexCommandResult GuardError;
		if (RejectBlockedGraphMutation(Params, GuardError))
		{
			// A patch caller always gets the compact result. The guard runs before any load, so the
			// refusal states no fingerprint, dirty state, mapping or locator: those are unknown.
			if (Command == TEXT("apply_patch")) GuardError.ErrorDetails = MakeUnloadedRefusalJson(Params, GuardError);
			return GuardError;
		}
	}

	if (Command == TEXT("list_graphs"))
	{
		return FCortexGraphNodeOps::ListGraphs(Params);
	}
	if (Command == TEXT("search_nodes"))
	{
		return FCortexGraphNodeOps::SearchNodes(Params);
	}
	if (Command == TEXT("trace_exec"))
	{
		return FCortexGraphTraceOps::TraceExec(Params);
	}
	if (Command == TEXT("trace_dataflow"))
	{
		return FCortexGraphTraceOps::TraceDataflow(Params);
	}
	if (Command == TEXT("get_subgraph"))
	{
		return FCortexGraphTraceOps::GetSubgraph(Params);
	}
	if (Command == TEXT("list_event_handlers"))
	{
		return FCortexGraphTraceOps::ListEventHandlers(Params);
	}
	if (Command == TEXT("find_event_handler"))
	{
		return FCortexGraphTraceOps::FindEventHandler(Params);
	}
	if (Command == TEXT("find_function_calls"))
	{
		return FCortexGraphTraceOps::FindFunctionCalls(Params);
	}
	if (Command == TEXT("add_node"))
	{
		return FCortexGraphNodeOps::AddNode(Params);
	}
	if (Command == TEXT("describe_node"))
	{
		return FCortexGraphNodeOps::DescribeNode(Params);
	}
	if (Command == TEXT("remove_node"))
	{
		return FCortexGraphNodeOps::RemoveNode(Params);
	}
	if (Command == TEXT("connect"))
	{
		return FCortexGraphConnectionOps::Connect(Params);
	}
	if (Command == TEXT("disconnect"))
	{
		return FCortexGraphConnectionOps::Disconnect(Params);
	}
	if (Command == TEXT("set_pin_value"))
	{
		return FCortexGraphNodeOps::SetPinValue(Params);
	}
	if (Command == TEXT("auto_layout"))
	{
		return FCortexGraphNodeOps::AutoLayout(Params);
	}
	if (Command == TEXT("apply_patch"))
	{
		return HandleApplyPatch(Params);
	}
	if (Command == TEXT("describe_node"))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::UnsupportedOperation,
			TEXT("graph.describe_node behavior is implemented by feat/safe-graph-authoring-recovery")
		);
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown graph command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexGraphCommandHandler::GetSupportedCommands() const
{
	return {
		FCortexCommandInfo{ TEXT("get_authoring_context"), TEXT("Inspect Blueprint authoring context, candidate graphs, and current authoring fingerprint") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Optional(TEXT("target"), TEXT("object"), TEXT("Optional target graph locator (graph_ref)")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("list_graphs"), TEXT("List user-visible Blueprint graphs with kind metadata and owning_interface for interface_impl graphs") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Optional(TEXT("include_subgraphs"), TEXT("boolean"), TEXT("Include composite subgraphs with parent_graph and subgraph_path fields")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("search_nodes"), TEXT("Search nodes across graphs by class, function name, or display name") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Optional(TEXT("node_class"), TEXT("string"), TEXT("Runtime node class filter"))
			.Optional(TEXT("function_name"), TEXT("string"), TEXT("Function-name filter for call nodes"))
			.Optional(TEXT("display_name"), TEXT("string"), TEXT("Node display-name filter"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Restrict search to a specific graph"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path to restrict search"))
			.Optional(TEXT("compact"), TEXT("boolean"), TEXT("Omit node_class from results (default: true)")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("trace_exec"), TEXT("Trace execution flow from a starting node") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("start_node_id"), TEXT("string"), TEXT("Identifier of the starting node"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing the node"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path"))
			.Optional(TEXT("max_depth"), TEXT("number"), TEXT("Maximum traversal depth"))
			.Optional(TEXT("traverse_policy"), TEXT("string"), TEXT("Traversal policy hint"))
			.Optional(TEXT("include_edges"), TEXT("boolean"), TEXT("Include traced edge list")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("trace_dataflow"), TEXT("Trace data-flow from a starting node") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("start_node_id"), TEXT("string"), TEXT("Identifier of the starting node"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing the node"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path"))
			.Optional(TEXT("max_depth"), TEXT("number"), TEXT("Maximum traversal depth"))
			.Optional(TEXT("traverse_policy"), TEXT("string"), TEXT("Traversal policy hint"))
			.Optional(TEXT("include_edges"), TEXT("boolean"), TEXT("Include traced edge list")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("get_subgraph"), TEXT("Read a graph or selected node subset with optional edges") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph to inspect, defaults to EventGraph"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path"))
			.Optional(TEXT("node_ids"), TEXT("array"), TEXT("Optional subset of node identifiers"))
			.Optional(TEXT("include_edges"), TEXT("boolean"), TEXT("Include edges between returned nodes")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("list_event_handlers"), TEXT("List event entry nodes across Blueprint graphs") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("find_event_handler"), TEXT("Find matching event entry nodes across graphs") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("event_name"), TEXT("string"), TEXT("Event display name or identifier to match")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("find_function_calls"), TEXT("Find call-function nodes by function name") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("function_name"), TEXT("string"), TEXT("Function-name filter")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("add_node"), TEXT("Add a node to a mutable graph. Delegate graphs are readable but not mutable.") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("node_class"), TEXT("string"), TEXT("Node class to create"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Target graph, defaults to EventGraph"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')"))
			.Optional(TEXT("position"), TEXT("object"), TEXT("Optional node placement coordinates"))
			.Optional(TEXT("params"), TEXT("object"), TEXT("Node-specific creation parameters")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("describe_node"), TEXT("Return the typed construction contract for a supported node class") }
			.Required(TEXT("node_class"), TEXT("string"), TEXT("Node class to describe"))
			.Optional(TEXT("params"), TEXT("object"), TEXT("Optional construction params used to allocate expected pins")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("remove_node"), TEXT("Remove a node from a mutable graph and clean up connections. Delegate graphs are readable but not mutable.") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Identifier of the node to remove"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing the node"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')")),
		FCortexCommandInfo{ TEXT("connect"), TEXT("Connect two pins in a mutable graph. Delegate graphs are readable but not mutable.") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("source_node"), TEXT("string"), TEXT("Node ID of the output node"))
			.Required(TEXT("source_pin"), TEXT("string"), TEXT("Output pin name"))
			.Required(TEXT("target_node"), TEXT("string"), TEXT("Node ID of the input node"))
			.Required(TEXT("target_pin"), TEXT("string"), TEXT("Input pin name"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing both nodes"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')")).RollbackSafe(),
		FCortexCommandInfo{ TEXT("disconnect"), TEXT("Disconnect a pin in a mutable graph. Delegate graphs are readable but not mutable.") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node containing the pin"))
			.Required(TEXT("pin_name"), TEXT("string"), TEXT("Pin to disconnect"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing the node"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')")),
		FCortexCommandInfo{ TEXT("set_pin_value"), TEXT("Set an input pin default in a mutable graph. Delegate graphs are readable but not mutable. Non-text pins use value; FText pins may use canonical text descriptor and are verified after save/reload.") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node containing the pin"))
			.Required(TEXT("pin_name"), TEXT("string"), TEXT("Input pin to modify"))
			.Optional(TEXT("value"), TEXT("string"), TEXT("Raw literal pin default for non-text pins or literal FText"))
			.Optional(TEXT("text"), TEXT("object"), TEXT("Canonical FText descriptor for FText pins"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing the node"))
			.Optional(TEXT("graph_kind"), TEXT("string"), TEXT("Graph kind disambiguator for persisted writes"))
			.Optional(TEXT("owning_interface"), TEXT("string"), TEXT("Interface owner disambiguator for interface implementation graphs"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')"))
			.Optional(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Optional stale-write guard")),
		FCortexCommandInfo{ TEXT("auto_layout"), TEXT("Auto-arrange nodes in mutable Blueprint graphs for readability. Delegate graphs are readable but not mutable.") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Specific graph to layout"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')")),
		FCortexCommandInfo{ TEXT("apply_patch"), TEXT("Preview or apply one typed graph patch as a single verified transaction: reversible mutation, one target compile, authoritative readback and optional explicit persistence. Two mutually exclusive shells share the envelope: the authoring shell (nodes/connections/pin_updates) and the migration shell (migration.op=\"replace_entry\"), which replaces a stale inherited implementation entry with the entry for the target declaration while preserving the downstream body. Standalone command: it cannot run inside a rollback-enabled batch because an outer batch cannot undo its compile or save boundary. Limits: max_nodes=64, max_edges=256, max_client_id_length=32, max_request_size_bytes=65536, max_scanned_nodes=2048.") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset; the patch request must name the same asset"))
			.Required(TEXT("target"), TEXT("object"), TEXT("Target locator: graph_ref with a canonical graph_guid (plus optional subgraph_path) or implementation with owner_class and function_name"))
			.Required(TEXT("patch_id"), TEXT("string"), TEXT("Caller-generated UUID that deterministically derives the identity of every new node"))
			.Required(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Stale-write guard copied from graph.get_authoring_context: graph_authoring_version 1 plus graph_authoring_hash, package_saved_hash, is_dirty, dirty_epoch and not_ready"))
			.Optional(TEXT("nodes"), TEXT("array"), TEXT("Authoring shell only: nodes to create or select, each with client_id, node_class, optional params, tagged defaults and position. node_class accepts a canonical class path or a family identifier. Supported families: CallFunction, VariableGet, VariableSet, Self, DynamicCast, ConstructObject, Event. Required for an authoring request (migration absent) and absent or empty for a migration request."))
			.Optional(TEXT("connections"), TEXT("array"), TEXT("Authoring shell only: edges between planned client ids or existing node GUIDs: from and to each carry client_id, node_guid or entry, plus pin. Required for an authoring request and absent or empty for a migration request."))
			.Optional(TEXT("pin_updates"), TEXT("array"), TEXT("Authoring shell only: existing input pin defaults to rewrite, each with node_guid, pin and a tagged default. Absent or empty for a migration request."))
			.Optional(TEXT("migration"), TEXT("object"), TEXT("Migration shell selector: {\"op\":\"replace_entry\",\"source\":{\"graph_ref\":{graph_guid,graph_kind?,subgraph_path?},\"entry_node_guid\":\"<guid>\"},\"pin_map\":[{\"entry\":\"input\"|\"output\",\"from_pin\":\"<stale pin>\",\"to_pin\":\"<replacement pin>\"}],\"remove_shadowing_member\":false}. It requires target.implementation and refuses any non-empty nodes/connections/pin_updates."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Strict JSON boolean (numeric or string values are refused, never coerced). Preview only, and never mutates, compiles or saves (default: true); an apply needs the expected_validation_hash returned by a preview"))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Strict JSON boolean (never coerced). Compile the target once after a reversible apply (default: true)"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Strict JSON boolean (never coerced). Persist the target package after verified readback (default: false); requires compile=true and a clean starting package"))
			.Optional(TEXT("allow_noop"), TEXT("boolean"), TEXT("Strict JSON boolean (never coerced). Permit an explicit change-free patch instead of refusing it (default: false)"))
			.Optional(TEXT("expected_validation_hash"), TEXT("string"), TEXT("validation_hash returned by a preview; required when dry_run is false")),
	};
}
