#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPAnalysisOps.h"
#include "Operations/CortexBPCleanupOps.h"
#include "Operations/CortexBPClassDefaultsOps.h"
#include "Operations/CortexBPCompareOps.h"
#include "Operations/CortexBPComponentOps.h"
#include "Operations/CortexBPGraphCleanupOps.h"
#include "Operations/CortexBPRedirectorOps.h"
#include "Operations/CortexBPSearchOps.h"
#include "Operations/CortexBPStructureOps.h"
#include "Operations/CortexBPTimelineOps.h"
#include "Operations/CortexBPClassSettingsOps.h"

FCortexCommandResult FCortexBPCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

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

	if (Command == TEXT("add_scs_component"))
	{
		return FCortexBPComponentOps::AddSCSComponent(Params);
	}

	if (Command == TEXT("analyze_for_migration"))
	{
		return FCortexBPAnalysisOps::AnalyzeForMigration(Params);
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

	if (Command == TEXT("reparent"))
	{
		return FCortexBPAssetOps::Reparent(Params);
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
		.Optional(TEXT("type"), TEXT("string"), TEXT("Optional Blueprint type filter")));
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
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path")));
	Commands.Add(FCortexCommandInfo{TEXT("save"), TEXT("Save a Blueprint")}
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
	Commands.Add(FCortexCommandInfo{TEXT("remove_graph"), TEXT("Remove a graph (function, macro, event graph) or custom event from a Blueprint")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("name"), TEXT("string"), TEXT("Name of graph or custom event to remove"))
		.Optional(TEXT("cascade_exec_chain"), TEXT("boolean"), TEXT("Remove connected execution chain for custom events (default: false)"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after removal (default: true)"))
		.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview what would be removed without modifying anything")));
	Commands.Add(FCortexCommandInfo{TEXT("get_class_defaults"), TEXT("Read default property values from a Blueprint CDO")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("properties"), TEXT("array"), TEXT("Specific properties to read"))
		.Optional(TEXT("blueprint_path"), TEXT("string"), TEXT("Optional alternate Blueprint path")));
	Commands.Add(FCortexCommandInfo{TEXT("set_class_defaults"), TEXT("Set default property values on a Blueprint CDO")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("properties"), TEXT("object"), TEXT("Class default values to set"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after applying defaults"))
		.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after applying defaults"))
		.Optional(TEXT("blueprint_path"), TEXT("string"), TEXT("Optional alternate Blueprint path")));
	Commands.Add(FCortexCommandInfo{TEXT("configure_timeline"), TEXT("Configure a Timeline node's tracks and keyframes")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("timeline_name"), TEXT("string"), TEXT("Timeline variable name"))
		.Required(TEXT("length"), TEXT("number"), TEXT("Timeline length in seconds"))
		.Optional(TEXT("loop"), TEXT("boolean"), TEXT("Loop the timeline"))
		.Optional(TEXT("tracks"), TEXT("array"), TEXT("Track and keyframe definitions")));
	Commands.Add(FCortexCommandInfo{TEXT("set_component_defaults"), TEXT("Set object-reference properties on a Blueprint component template")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("component_name"), TEXT("string"), TEXT("Component template name"))
		.Required(TEXT("properties"), TEXT("object"), TEXT("Properties to apply")));
	Commands.Add(FCortexCommandInfo{TEXT("add_scs_component"), TEXT("Add an SCS component to a Blueprint")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("component_class"), TEXT("string"), TEXT("Component class name (e.g. StaticMeshComponent)"))
		.Optional(TEXT("component_name"), TEXT("string"), TEXT("Variable name (auto-generated if omitted)"))
		.Optional(TEXT("parent_component"), TEXT("string"), TEXT("Parent SCS component variable name"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after adding")));
	Commands.Add(FCortexCommandInfo{TEXT("analyze_for_migration"), TEXT("Analyze a Blueprint for C++ migration")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path")));
	Commands.Add(FCortexCommandInfo{TEXT("cleanup_migration"), TEXT("Clean up a Blueprint after C++ migration")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Optional(TEXT("new_parent_class"), TEXT("string"), TEXT("New C++ parent class"))
		.Optional(TEXT("remove_variables"), TEXT("array"), TEXT("Variables to remove"))
		.Optional(TEXT("remove_functions"), TEXT("array"), TEXT("Functions to remove"))
		.Optional(TEXT("migrated_overrides"), TEXT("array"), TEXT("Overrides already migrated"))
		.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after cleanup")));
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
	Commands.Add(FCortexCommandInfo{TEXT("reparent"), TEXT("Reparent a Blueprint to a new parent class")}
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
		.Required(TEXT("new_parent"), TEXT("string"), TEXT("New parent class (Blueprint path or C++ class name)")));
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
