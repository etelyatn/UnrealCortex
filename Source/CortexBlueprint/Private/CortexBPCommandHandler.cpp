#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPAnalysisOps.h"
#include "Operations/CortexBPCleanupOps.h"
#include "Operations/CortexBPClassDefaultsOps.h"
#include "Operations/CortexBPCompileAllOps.h"
#include "Operations/CortexBPMigrationExportOps.h"
#include "Operations/CortexBPCompareOps.h"
#include "Operations/CortexBPComponentOps.h"
#include "Operations/CortexBPGraphCleanupOps.h"
#include "Operations/CortexBPAdoptMemberOps.h"
#include "Operations/CortexBPGraphMetricsOps.h"
#include "Operations/CortexBPRedirectorOps.h"
#include "Operations/CortexBPReparentOps.h"
#include "Operations/CortexBPRenameMemberOps.h"
#include "Operations/CortexBPSearchOps.h"
#include "Operations/CortexBPStructureOps.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "Operations/CortexBPTextInventoryOps.h"
#include "Operations/CortexBPTimelineOps.h"
#include "Operations/CortexBPClassSettingsOps.h"
#include "CortexAssetMutationGuard.h"
#include "Engine/Blueprint.h"

namespace
{
bool RejectBlockedBlueprintMutation(const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& OutError)
{
	FString AssetPath;
	if (!Params.IsValid()
		|| (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)
			&& !Params->TryGetStringField(TEXT("source_path"), AssetPath)
			&& !Params->TryGetStringField(TEXT("blueprint_path"), AssetPath)))
	{
		return false;
	}
	FString Reason;
	if (FCortexAssetMutationGuard::IsPathBlocked(AssetPath, Reason))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *Reason));
		return true;
	}
	return false;
}
}

FCortexCommandResult FCortexBPCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

	const bool bReadOnly = Command == TEXT("list") || Command == TEXT("get_info")
		|| Command == TEXT("get_class_defaults") || Command == TEXT("list_inherited_properties")
		|| Command == TEXT("list_settable_defaults") || Command == TEXT("list_scs_components")
		|| Command == TEXT("analyze_for_migration") || Command == TEXT("compare_blueprints")
		|| Command == TEXT("search") || Command == TEXT("graph_metrics") || Command == TEXT("export_text_inventory")
		|| Command == TEXT("export_snapshot") || Command == TEXT("compare_snapshots");
	if (!bReadOnly)
	{
		FCortexCommandResult GuardError;
		if (RejectBlockedBlueprintMutation(Params, GuardError)) return GuardError;
	}
	if (Command == TEXT("create"))
	{
		return FCortexBPAssetOps::Create(Params);
	}

	if (Command == TEXT("list"))
	{
		return FCortexBPAssetOps::List(Params);
	}

	if (Command == TEXT("get_info"))
	{
		return FCortexBPAssetOps::GetInfo(Params);
	}

	if (Command == TEXT("delete"))
	{
		return FCortexBPAssetOps::Delete(Params);
	}

	if (Command == TEXT("duplicate"))
	{
		return FCortexBPAssetOps::Duplicate(Params);
	}

	if (Command == TEXT("compile"))
	{
		return FCortexBPAssetOps::Compile(Params);
	}

	if (Command == TEXT("save"))
	{
		return FCortexBPAssetOps::Save(Params);
	}

	if (Command == TEXT("rename"))
	{
		return FCortexBPAssetOps::Rename(Params);
	}

	// Structure operations
	if (Command == TEXT("add_variable"))
	{
		return FCortexBPStructureOps::AddVariable(Params);
	}

	if (Command == TEXT("remove_variable"))
	{
		return FCortexBPStructureOps::RemoveVariable(Params);
	}

	if (Command == TEXT("add_function"))
	{
		return FCortexBPStructureOps::AddFunction(Params);
	}

	if (Command == TEXT("remove_graph"))
	{
		return FCortexBPStructureOps::RemoveGraph(Params);
	}

	if (Command == TEXT("get_class_defaults"))
	{
		return FCortexBPClassDefaultsOps::GetClassDefaults(Params);
	}

	if (Command == TEXT("list_inherited_properties"))
	{
		return FCortexBPClassDefaultsOps::ListInheritedProperties(Params);
	}

	if (Command == TEXT("list_settable_defaults"))
	{
		return FCortexBPClassDefaultsOps::ListSettableDefaults(Params);
	}

	if (Command == TEXT("set_class_defaults"))
	{
		return FCortexBPClassDefaultsOps::SetClassDefaults(Params);
	}

	if (Command == TEXT("configure_timeline"))
	{
		return FCortexBPTimelineOps::ConfigureTimeline(Params);
	}

	if (Command == TEXT("set_component_defaults"))
	{
		return FCortexBPComponentOps::SetComponentDefaults(Params);
	}

	if (Command == TEXT("list_scs_components"))
	{
		return FCortexBPComponentOps::ListSCSComponents(Params);
	}

	if (Command == TEXT("add_scs_component"))
	{
		return FCortexBPComponentOps::AddSCSComponent(Params);
	}

	if (Command == TEXT("analyze_for_migration"))
	{
		return FCortexBPAnalysisOps::AnalyzeForMigration(Params);
	}

	if (Command == TEXT("graph_metrics"))
	{
		return FCortexBPGraphMetricsOps::GraphMetrics(Params);
	}

	if (Command == TEXT("cleanup_migration"))
	{
		return FCortexBPCleanupOps::CleanupMigration(Params);
	}

	if (Command == TEXT("remove_scs_component"))
	{
		return FCortexBPCleanupOps::RemoveSCSComponent(Params);
	}

	if (Command == TEXT("rename_scs_component"))
	{
		return FCortexBPCleanupOps::RenameSCSComponent(Params);
	}

	if (Command == TEXT("recompile_dependents"))
	{
		return FCortexBPCleanupOps::RecompileDependents(Params);
	}

	if (Command == TEXT("fixup_redirectors"))
	{
		return FCortexBPRedirectorOps::FixupRedirectors(Params);
	}

	if (Command == TEXT("compare_blueprints"))
	{
		return FCortexBPCompareOps::CompareBlueprints(Params);
	}

	if (Command == TEXT("delete_orphaned_nodes"))
	{
		return FCortexBPGraphCleanupOps::DeleteOrphanedNodes(Params);
	}

	if (Command == TEXT("search"))
	{
		return FCortexBPSearchOps::Search(Params);
	}

	if (Command == TEXT("export_text_inventory"))
	{
		return FCortexBPTextInventoryOps::ExportTextInventory(Params);
	}

	if (Command == TEXT("export_snapshot"))
	{
		return FCortexBPSnapshotOps::ExportSnapshot(Params);
	}

	if (Command == TEXT("compare_snapshots"))
	{
		return FCortexBPSnapshotOps::CompareSnapshots(Params);
	}

	if (Command == TEXT("compile_all"))
	{
		return FCortexBPCompileAllOps::CompileAll(Params);
	}

	if (Command == TEXT("export_migration_analysis"))
	{
		return FCortexBPMigrationExportOps::ExportMigrationAnalysis(Params);
	}

	if (Command == TEXT("reparent"))
	{
		return FCortexBPReparentOps::Execute(Params);
	}

	if (Command == TEXT("rename_member"))
	{
		return FCortexBPRenameMemberOps::Execute(Params);
	}

	if (Command == TEXT("adopt_native_member"))
	{
		return FCortexBPAdoptMemberOps::Execute(Params);
	}

	if (Command == TEXT("add_interface"))
	{
		return FCortexBPClassSettingsOps::AddInterface(Params);
	}

	if (Command == TEXT("remove_interface"))
	{
		return FCortexBPClassSettingsOps::RemoveInterface(Params);
	}

	if (Command == TEXT("set_tick_settings"))
	{
		return FCortexBPClassSettingsOps::SetTickSettings(Params);
	}

	if (Command == TEXT("set_replication_settings"))
	{
		return FCortexBPClassSettingsOps::SetReplicationSettings(Params);
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown bp command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexBPCommandHandler::GetSupportedCommands() const
{
	TArray<FCortexCommandInfo> Commands;

	Commands.Add({TEXT("create"), TEXT("Create a new Blueprint asset")});
	Commands.Last()
		.Required(TEXT("name"), TEXT("string"), TEXT("Blueprint asset name"))
		.Required(TEXT("path"), TEXT("string"), TEXT("Destination content path"))
		.Required(TEXT("type"), TEXT("string"), TEXT("Blueprint type"))
		.Required(TEXT("parent_class"), TEXT("string"), TEXT("Parent class name"));
	Commands.Add(FCortexCommandInfo{TEXT("list"), TEXT("List Blueprint assets")}
		.Optional(TEXT("path"), TEXT("string"), TEXT("Optional content path filter"))
		.Optional(TEXT("type"), TEXT("string"),
			TEXT("Optional Blueprint type filter (case-insensitive): Interface, FunctionLibrary, MacroLibrary, AnimBlueprint, EditorUtility, Widget, Component, Actor, Object, Unknown"))
		.Optional(TEXT("exclude_types"), TEXT("array"),
			TEXT("Optional array of Blueprint types to omit (same vocabulary as type); combines with type")));
	Commands.Add(FCortexCommandInfo{TEXT("get_info"), TEXT("Get Blueprint info")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("include_inherited"), TEXT("boolean"), TEXT("Include inherited C++ functions (default: true)"))
		.Optional(TEXT("compact"), TEXT("boolean"), TEXT("Omit empty inputs/outputs arrays and source field (default: true)")));
	Commands.Add(FCortexCommandInfo{TEXT("delete"), TEXT("Delete a Blueprint asset")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("force"), TEXT("boolean"), TEXT("Delete without additional confirmation")));
	Commands.Add(FCortexCommandInfo{TEXT("duplicate"), TEXT("Duplicate a Blueprint asset")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Source Blueprint asset path"))
		.Required(TEXT("new_name"), TEXT("string"), TEXT("New Blueprint asset name"))
		.Required(TEXT("new_path"), TEXT("string"), TEXT("Destination content path")));
	Commands.Add(FCortexCommandInfo{TEXT("compile"), TEXT("Compile a Blueprint")}
		.OptionalBatchItems(TEXT("Batch items with target and expected_fingerprint"))
		.OptionalExpectedFingerprint()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path")));
	Commands.Add(FCortexCommandInfo{TEXT("save"), TEXT("Save a Blueprint")}
		.OptionalBatchItems(TEXT("Batch items with target and expected_fingerprint"))
		.OptionalExpectedFingerprint()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path")));
	Commands.Add(FCortexCommandInfo{TEXT("rename"), TEXT("Rename/move a Blueprint asset")}
		.Required(TEXT("source_path"), TEXT("string"), TEXT("Source Blueprint asset path"))
		.Required(TEXT("dest_path"), TEXT("string"), TEXT("Destination Blueprint asset path")));
	Commands.Add(FCortexCommandInfo{TEXT("add_variable"), TEXT("Add a variable to a Blueprint")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("name"), TEXT("string"), TEXT("Variable name"))
		.Required(TEXT("type"), TEXT("string"), TEXT("Variable type"))
		.Optional(TEXT("default_value"), TEXT("string"), TEXT("Optional default value"))
		.Optional(TEXT("is_exposed"), TEXT("boolean"), TEXT("Expose on spawn / instance"))
		.Optional(TEXT("category"), TEXT("string"), TEXT("Variable category")));
	Commands.Add(FCortexCommandInfo{TEXT("remove_variable"), TEXT("Remove a variable from a Blueprint")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("name"), TEXT("string"), TEXT("Variable name")));
	Commands.Add(FCortexCommandInfo{TEXT("add_function"), TEXT("Add a function to a Blueprint")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("name"), TEXT("string"), TEXT("Function name"))
		.Optional(TEXT("is_pure"), TEXT("boolean"), TEXT("Create as a pure function"))
		.Optional(TEXT("access"), TEXT("string"), TEXT("Function access level"))
		.Optional(TEXT("inputs"), TEXT("array"), TEXT("Input parameter definitions"))
		.Optional(TEXT("outputs"), TEXT("array"), TEXT("Output parameter definitions")));
	Commands.Add(FCortexCommandInfo{
		TEXT("remove_graph"),
		TEXT("Preview or remove a graph/custom event with explicit compile/save lifecycle") }
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("name"), TEXT("string"), TEXT("Graph or custom-event name selector"))
		.Required(TEXT("dry_run"), TEXT("boolean"), TEXT("true=preview, false=apply"))
		.Required(TEXT("compile"), TEXT("boolean"), TEXT("Compile after apply"))
		.Required(TEXT("save"), TEXT("boolean"), TEXT("Persist the verified result after compile/readback"))
		.Optional(TEXT("cascade_exec_chain"), TEXT("boolean"), TEXT("Custom-event exec cascade, default false"))
		.OptionalExpectedFingerprint()
		.Optional(TEXT("expected_validation_hash"), TEXT("string"), TEXT("Validation hash returned by preview")));
	Commands.Add(FCortexCommandInfo{TEXT("get_class_defaults"), TEXT("Read a Blueprint CDO's default properties (metadata-only discovery unless values requested)")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("properties"), TEXT("array"), TEXT("Specific property paths to read; nonempty defaults to effective values"))
		.Optional(TEXT("include_values"), TEXT("boolean"), TEXT("Include effective values; default true for a selected read, false for discovery"))
		.Optional(TEXT("include_members"), TEXT("boolean"), TEXT("Expand one level of struct member metadata (values only with include_values)"))
		.Optional(TEXT("changed_only"), TEXT("boolean"), TEXT("Return only defaults differing from the parent class baseline (inherited delta); requires include_values and excludes include_members; an incomplete comparison is flagged, never reported as equal"))
		.Optional(TEXT("discovery_offset"), TEXT("number"), TEXT("Metadata discovery page offset; default 0"))
		.Optional(TEXT("discovery_limit"), TEXT("number"), TEXT("Metadata discovery page size; default 100, max 200"))
		.Optional(TEXT("blueprint_path"), TEXT("string"), TEXT("Optional alternate Blueprint path")));
	Commands.Add(FCortexCommandInfo{TEXT("list_inherited_properties"), TEXT("List inherited class-default property metadata (paged)")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("properties"), TEXT("array"), TEXT("Specific property paths to select"))
		.Optional(TEXT("include_values"), TEXT("boolean"), TEXT("Include effective values; default false"))
		.Optional(TEXT("include_members"), TEXT("boolean"), TEXT("Expand one level of struct member metadata (values only with include_values)"))
		.Optional(TEXT("discovery_offset"), TEXT("number"), TEXT("Metadata discovery page offset; default 0"))
		.Optional(TEXT("discovery_limit"), TEXT("number"), TEXT("Metadata discovery page size; default 100, max 200")));
	Commands.Add(FCortexCommandInfo{TEXT("list_settable_defaults"), TEXT("List settable class-default property metadata (paged)")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("properties"), TEXT("array"), TEXT("Specific property paths to select"))
		.Optional(TEXT("include_values"), TEXT("boolean"), TEXT("Include effective values; default false"))
		.Optional(TEXT("include_members"), TEXT("boolean"), TEXT("Expand one level of struct member metadata (values only with include_values)"))
		.Optional(TEXT("discovery_offset"), TEXT("number"), TEXT("Metadata discovery page offset; default 0"))
		.Optional(TEXT("discovery_limit"), TEXT("number"), TEXT("Metadata discovery page size; default 100, max 200")));
	Commands.Add(FCortexCommandInfo{TEXT("set_class_defaults"), TEXT("Set default property values on a Blueprint CDO")}
		.OptionalBatchItems(TEXT("Batch items with target, properties, compile, save, echo, expected_fingerprint"))
		.OptionalExpectedFingerprint()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("properties"), TEXT("object"), TEXT("Class default values to set"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after applying defaults"))
		.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after applying defaults"))
		.Optional(TEXT("echo"), TEXT("string"), TEXT("Value echo mode: diff (default, changed leaves only) | full (complete previous/new) | none (outcome only)"))
		.Optional(TEXT("blueprint_path"), TEXT("string"), TEXT("Optional alternate Blueprint path")));
	Commands.Add(FCortexCommandInfo{TEXT("configure_timeline"), TEXT("Configure a Timeline node's tracks and keyframes")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("timeline_name"), TEXT("string"), TEXT("Timeline variable name"))
		.Required(TEXT("length"), TEXT("number"), TEXT("Timeline length in seconds"))
		.Optional(TEXT("loop"), TEXT("boolean"), TEXT("Loop the timeline"))
		.Optional(TEXT("tracks"), TEXT("array"), TEXT("Track and keyframe definitions")));
	Commands.Add(FCortexCommandInfo{TEXT("set_component_defaults"), TEXT("Set JSON-valued properties on an owned Blueprint SCS component template")}
		.OptionalBatchItems(TEXT("Batch items with target, component_name, properties, compile, save, expected_fingerprint"))
		.OptionalExpectedFingerprint()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("component_name"), TEXT("string"), TEXT("Owned SCS component template name from this Blueprint"))
		.Required(TEXT("properties"), TEXT("object"), TEXT("JSON property values. Supports StaticMesh object paths, OverrideMaterials[N], bools, numbers, enums, and structs such as RelativeLocation"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after applying successful property writes, default true"))
		.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after successful writes only when there are no partial failures, default false")));
	Commands.Add(FCortexCommandInfo{TEXT("list_scs_components"), TEXT("List Blueprint SCS components in writer-ready reference form")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path")));
	Commands.Add(FCortexCommandInfo{TEXT("add_scs_component"), TEXT("Add an SCS component to a Blueprint")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("component_class"), TEXT("string"), TEXT("Component class name (e.g. StaticMeshComponent)"))
		.Optional(TEXT("component_name"), TEXT("string"), TEXT("Variable name (auto-generated if omitted)"))
		.Optional(TEXT("parent_component"), TEXT("string"), TEXT("Parent SCS component variable name"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after adding")));
	Commands.Add(FCortexCommandInfo{TEXT("analyze_for_migration"), TEXT("Analyze a Blueprint for C++ migration")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("sections"), TEXT("array"),
			TEXT("Optional section names to return (omitted or empty = all). Identity fields name, asset_path, type, "
				"parent_class, native_parent_class, parent_class_path are always included. Valid: is_compiled, variables, functions, "
				"scs_components, scs_collisions, dynamic_components, graphs, per_graph_elements, latent_nodes, "
				"timelines, event_dispatchers, interfaces_implemented, input_bindings, construction_script, widgets, "
				"named_slots, widget_animations, widget_bindings, referenced_user_types, cdo_overrides, "
				"instanced_subobjects, widget_dependencies, entity_summary, complexity_metrics")));
	Commands.Add(FCortexCommandInfo{TEXT("graph_metrics"),
		TEXT("Reachability-aware logic metrics per entry node (events, custom/bound/input events, functions): functional node count, conditionals, variable getter fan-out, unreachable nodes (inner macro/collapsed-graph orphans carry a subgraph field), disabled nodes (disabled_nodes), ghost entries (unlinked or disabled), has_tick for live Tick events, and threshold violations")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("thresholds"), TEXT("object"), TEXT("{max_nodes_per_entry: int=10, max_nodes_per_blueprint: int=30}; positive integers")));
	Commands.Add(FCortexCommandInfo{TEXT("cleanup_migration"), TEXT("Clean up a Blueprint after C++ migration")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("new_parent_class"), TEXT("string"), TEXT("New C++ parent class"))
		.Optional(TEXT("remove_variables"), TEXT("array"), TEXT("Variables to remove"))
		.Optional(TEXT("remove_functions"), TEXT("array"), TEXT("Functions to remove"))
		.Optional(TEXT("migrated_overrides"), TEXT("array"), TEXT("Overrides already migrated"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after cleanup (default true)"))
		.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the whole package, including pre-existing dirty edits (default true); false stages changes in memory. Compiler errors prevent saving.")));
	Commands.Add(FCortexCommandInfo{TEXT("remove_scs_component"), TEXT("Remove an SCS component node from a Blueprint (use after migrating to C++ UPROPERTY)")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("component_name"), TEXT("string"), TEXT("SCS component node name"))
		.Optional(TEXT("acknowledged_losses"), TEXT("array"), TEXT("Keys from required_acknowledgment to confirm sub-object loss"))
		.Optional(TEXT("force"), TEXT("boolean"), TEXT("Override dirty-state protection and remove anyway"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after removal")));
	Commands.Add(FCortexCommandInfo{TEXT("rename_scs_component"), TEXT("Rename an SCS component node on a Blueprint")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("old_name"), TEXT("string"), TEXT("Current SCS component node name"))
		.Required(TEXT("new_name"), TEXT("string"), TEXT("New SCS component node name"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after rename")));
	Commands.Add(FCortexCommandInfo{TEXT("recompile_dependents"), TEXT("Recompile Blueprints that depend on a target Blueprint")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path")));
	Commands.Add(FCortexCommandInfo{TEXT("fixup_redirectors"), TEXT("Fix up redirectors under a content path")}
		.Required(TEXT("path"), TEXT("string"), TEXT("Content path to scan"))
		.Optional(TEXT("recursive"), TEXT("boolean"), TEXT("Recurse into subfolders")));
	Commands.Add(FCortexCommandInfo{TEXT("compare_blueprints"), TEXT("Compare two Blueprints and return structural differences")}
		.Required(TEXT("source_path"), TEXT("string"), TEXT("Source Blueprint asset path"))
		.Required(TEXT("target_path"), TEXT("string"), TEXT("Target Blueprint asset path"))
		.Optional(TEXT("sections"), TEXT("array"), TEXT("Sections to compare")));
	Commands.Add(FCortexCommandInfo{TEXT("delete_orphaned_nodes"), TEXT("Delete orphaned nodes from a Blueprint graph")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Optional graph name"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after cleanup")));
	Commands.Add(FCortexCommandInfo{TEXT("search"), TEXT("Search a Blueprint for values across graphs, class defaults, and widget tree")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("query"), TEXT("string"), TEXT("Search text"))
		.Optional(TEXT("search_in"), TEXT("array"), TEXT("Search scopes"))
		.Optional(TEXT("case_sensitive"), TEXT("boolean"), TEXT("Case-sensitive matching"))
		.Optional(TEXT("max_results"), TEXT("number"), TEXT("Maximum matches to return")));
	Commands.Add(FCortexCommandInfo{TEXT("export_text_inventory"), TEXT("Export every FText of a Blueprint (class defaults, widgets, text pin defaults) as a sorted inventory with source kind and string table identity (read-only)")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("out_path"), TEXT("string"), TEXT("Write the full inventory JSON to this file (under Project Saved, Content or Config) and return only asset_path, out_path and counts; required when the inline response would exceed 40000 characters"))
		.StrictParams());
	Commands.Add(FCortexCommandInfo{TEXT("export_snapshot"), TEXT("Write a deterministic migration snapshot of a Blueprint (variables with their effective default read from the CDO, CDO with declaring class, components incl. this Blueprint's own overrides of inherited components, widgets, bindings, animations, text, interfaces, graph entries) to a file; returns only a compact summary (read-only)")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("out_path"), TEXT("string"), TEXT("File to write the snapshot JSON to (under Project Saved, Content or Config)"))
		.Optional(TEXT("sections"), TEXT("array"), TEXT("Sections to include (variables, cdo, components, widgets, bindings, animations, text, interfaces, graph_entries); default all"))
		.StrictParams());
	Commands.Add(FCortexCommandInfo{TEXT("compare_snapshots"), TEXT("Diff two Blueprint snapshot files by section and record identity into added/removed/changed/moved; a CDO property whose declaring class changed with an equal value is moved (decided on the value alone; a type change such as double to float is attached as type_change, float/double compared at float precision); refuses an empty compared set and duplicate record keys (read-only)")}
		.Required(TEXT("before"), TEXT("string"), TEXT("Snapshot file written before the change"))
		.Required(TEXT("after"), TEXT("string"), TEXT("Snapshot file written after the change"))
		.Optional(TEXT("report_path"), TEXT("string"), TEXT("Write the full report to this file (under Project Saved, Content or Config) and omit the inline difference list; required when the inline report would exceed 40000 characters"))
		.StrictParams());
	Commands.Add(FCortexCommandInfo{TEXT("compile_all"), TEXT("Compile every Blueprint of a folder (recursive) or an explicit asset list through the single-asset compile, write a per-asset report (status, error/warning counts, diagnostics) and return totals plus the first failing assets; read-only content and assets blocked after a failed recovery are skipped with a reason, level script Blueprints are not enumerated; synchronous, garbage is collected once at the end, large prefixes take minutes")}
		.Required(TEXT("out_path"), TEXT("string"), TEXT("File to write the full compile report JSON to (under Project Saved, Content or Config)"))
		.Optional(TEXT("path_prefix"), TEXT("string"), TEXT("Content folder compiled recursively (exactly one of path_prefix or paths)"))
		.Optional(TEXT("paths"), TEXT("array"), TEXT("Explicit Blueprint asset paths (exactly one of path_prefix or paths)"))
		.Optional(TEXT("types"), TEXT("array"), TEXT("Only compile these Blueprint types (case-insensitive, same vocabulary as list): Interface, FunctionLibrary, MacroLibrary, AnimBlueprint, EditorUtility, Widget, Component, Actor, Object, Unknown (from registry tags, no load)"))
		.StrictParams());
	Commands.Add(FCortexCommandInfo{TEXT("export_migration_analysis"), TEXT("Analyze every Blueprint of a folder (recursive) or an explicit asset list in one call with the same builders as analyze_for_migration and graph_metrics, and write a JSONL file (one {asset_path, analysis, graph_metrics} row per Blueprint, sorted by asset_path; error rows {asset_path, status, error_code, error_message} for failed, skipped and not found assets) plus <out_path without extension>.index.json (per exported Blueprint: name, asset_path, type, parent_class, native_parent_class, parent_chain = Blueprint ancestors immediate parent first ending before the first native class, metrics {functional_nodes, has_tick, unreachable_nodes, conditionals = sum over live entries}, violations, migration_confidence, unsupported_node_types, blockers = unsupported_nodes when unsupported_node_types is non-empty, latent_nodes, timelines, low_migration_confidence when migration_confidence is low; plus totals and by_type). Returns out_path, index_path, totals {total, exported, failed, not_found, skipped, excluded_by_type, matched_assets (Blueprints found under path_prefix before the types filter)}, by_type, the first failures and warnings (a path_prefix that matched no Blueprint). Index violations always use the default graph_metrics thresholds (max_nodes_per_entry 10, max_nodes_per_blueprint 30); this command has no thresholds parameter, call graph_metrics on single assets for custom thresholds. Assets are loaded but never modified or saved (only the two report files are written), assets blocked after a failed recovery are skipped with a reason, level script Blueprints are not enumerated; synchronous, garbage is collected once at the end, large prefixes take minutes")}
		.Required(TEXT("out_path"), TEXT("string"), TEXT("File to write the JSONL export to (under Project Saved, Content or Config); the index goes next to it as <name>.index.json under the same policy"))
		.Optional(TEXT("path_prefix"), TEXT("string"), TEXT("Content folder analysed recursively (exactly one of path_prefix or paths)"))
		.Optional(TEXT("paths"), TEXT("array"), TEXT("Explicit Blueprint asset paths (exactly one of path_prefix or paths); paths without a registered Blueprint become not_found rows"))
		.Optional(TEXT("types"), TEXT("array"), TEXT("Only export these Blueprint types (case-insensitive, same vocabulary as list): Interface, FunctionLibrary, MacroLibrary, AnimBlueprint, EditorUtility, Widget, Component, Actor, Object, Unknown (from registry tags, no load)"))
		.Optional(TEXT("sections"), TEXT("array"), TEXT("Documents to put in each row (omitted or empty = all): any analyze_for_migration section (identity fields are always present) plus graph_metrics; the index always carries its summary fields"))
		.StrictParams());
	Commands.Add(FCortexCommandInfo{TEXT("reparent"), TEXT("Reparent a Blueprint to a new parent class with a collision preflight (variables the engine replaces or renames, native function clashes, SCS collisions, SaveGame findings, interface overlap), a dry_run, the editor's reparent compile flags (the new native parent's defaults win), compile diagnostics, a CDO delta and an explicit save. Refuses an ambiguous class name (AMBIGUOUS_CLASS), a family change (INVALID_PARENT_CLASS), blocking collisions (NAME_COLLISION) and unacknowledged losses (POTENTIAL_DATA_LOSS with required_acknowledgment); a compile error or readback mismatch restores the Blueprint. reparented is true only after readback.")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("new_parent"), TEXT("string"), TEXT("New parent class: Blueprint asset path, full class path or unique short name"))
		.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("true = report the plan, mutate and dirty nothing (default false)"))
		.OptionalExpectedFingerprint(TEXT("Stale-write guard {graph_authoring_version, graph_authoring_hash, migration_state_hash}; read it from a dry run"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after the parent change with the editor's reparent flags (default true; required for save)"))
		.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the package after a verified result (default false; requires compile)"))
		.Optional(TEXT("acknowledged_collisions"), TEXT("array"), TEXT("Exactly the required_acknowledgment keys of the dry run (variable:<n>, scs:<n>, save_game:<n>)"))
		.Optional(TEXT("report_path"), TEXT("string"), TEXT("Write the full CDO delta to this file (under Project Saved, Content or Config) and omit the inline list"))
		.StrictParams());
	Commands.Add(FCortexCommandInfo{TEXT("rename_member"), TEXT("Rename a Blueprint variable, event dispatcher, function or function-local variable with its references, a dry_run listing the nodes that follow, an explicit compile/save and a fail-closed readback. Refuses (INVALID_OPERATION with the reference list) when another Blueprint (loaded, unsaved, a derived class or an Asset Registry referencer, Level Blueprints included) references the member, when the new name is taken in this Blueprint, its native parent or a child (NAME_COLLISION), for RepNotify/FieldNotify members, override/interface functions and component variables; a SaveGame variable needs acknowledged_losses=[save_game:<variable>] (POTENTIAL_DATA_LOSS)")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("kind"), TEXT("string"), TEXT("variable | function | dispatcher | local"))
		.Required(TEXT("old"), TEXT("string"), TEXT("Current member name"))
		.Required(TEXT("new"), TEXT("string"), TEXT("New member name"))
		.Optional(TEXT("function"), TEXT("string"), TEXT("Function graph that owns the variable; required for kind=local, invalid otherwise"))
		.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("true = report the plan, mutate and dirty nothing (default false)"))
		.OptionalExpectedFingerprint(TEXT("Stale-write guard {graph_authoring_version, graph_authoring_hash, migration_state_hash}; read it from a dry run"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after the rename (default true; required for save)"))
		.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the package after a verified result (default false; requires compile)"))
		.Optional(TEXT("acknowledged_losses"), TEXT("array"), TEXT("Exactly the required_acknowledgment keys of the dry run (save_game:<variable>)"))
		.StrictParams());
	Commands.Add(FCortexCommandInfo{TEXT("adopt_native_member"), TEXT("Hand Blueprint variables over to properties the native parent already declares: every get/set node and AnimGraph binding is retargeted without being destroyed, the variable is removed, the Blueprint is compiled once, and the old default, loaded placed-instance values and loaded child CDOs are carried to the native property, then read back fail-closed (READBACK_MISMATCH restores the Blueprint). Handles state variable_present and engine_already_adopted. Refuses unknown or non-native properties, incompatible types (INVALID_FIELD), a Blueprint that is not compiled (STALE_PRECONDITION), unmapped variables the parent already declares (NAME_COLLISION), renames without a property redirect (MISSING_PROPERTY_REDIRECT with the exact [CoreRedirects] line unless instance_values:<variable> is acknowledged), dependents that reference a renamed variable (INVALID_OPERATION) and SaveGame variables until acknowledged_losses carries save_game:<variable> (POTENTIAL_DATA_LOSS)")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("mappings"), TEXT("array"), TEXT("[{bp_variable, native_property, default?}]; default only for engine_already_adopted"))
		.Optional(TEXT("transfer_default"), TEXT("boolean"), TEXT("true = the old Blueprint default becomes the native default; false = the CDO takes the parent default (default true)"))
		.Optional(TEXT("retarget_nodes"), TEXT("boolean"), TEXT("Retarget nodes that name the variable (default true; false is refused for renames)"))
		.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("true = report the plan (state, type match, old default, nodes, loaded instances, SaveGame findings, redirect line), mutate and dirty nothing (default false)"))
		.OptionalExpectedFingerprint(TEXT("Stale-write guard {graph_authoring_version, graph_authoring_hash, migration_state_hash}; read it from a dry run"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after the adoption (default true; required for save, transfer_default=false and renames)"))
		.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the package after a verified result (default false; requires compile)"))
		.Optional(TEXT("acknowledged_losses"), TEXT("array"), TEXT("save_game:<variable> keys equal to the dry run's required_acknowledgment, plus instance_values:<variable> for a rename without a property redirect"))
		.StrictParams());
	Commands.Add(FCortexCommandInfo{TEXT("add_interface"), TEXT("Add an interface implementation to a Blueprint")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("interface_path"), TEXT("string"), TEXT("Interface class name or path"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after adding (default: true)")));
	Commands.Add(FCortexCommandInfo{TEXT("remove_interface"), TEXT("Remove an interface implementation from a Blueprint")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("interface_path"), TEXT("string"), TEXT("Interface class name or path"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after removing (default: true)")));
	Commands.Add(FCortexCommandInfo{TEXT("set_tick_settings"), TEXT("Set Actor tick settings on a Blueprint CDO")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("start_with_tick_enabled"), TEXT("boolean"), TEXT("Enable tick at start (also forces bCanEverTick=true when enabling)"))
		.Optional(TEXT("can_ever_tick"), TEXT("boolean"), TEXT("Whether actor can ever tick (independent of start_with_tick_enabled)"))
		.Optional(TEXT("tick_interval"), TEXT("number"), TEXT("Tick interval in seconds (0 = every frame)"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting (default: true)"))
		.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after setting (default: false)")));
	Commands.Add(FCortexCommandInfo{TEXT("set_replication_settings"), TEXT("Set replication settings on a Blueprint CDO")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("replicates"), TEXT("boolean"), TEXT("Enable replication"))
		.Optional(TEXT("replicate_movement"), TEXT("boolean"), TEXT("Replicate movement"))
		.Optional(TEXT("net_dormancy"), TEXT("string"), TEXT("Net dormancy: DORM_Never|DORM_Awake|DORM_DormantAll|DORM_DormantPartial|DORM_Initial"))
		.Optional(TEXT("net_use_owner_relevancy"), TEXT("boolean"), TEXT("Use owner relevancy"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting (default: true)"))
		.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after setting (default: false)")));

	return Commands;
}
