#include "CortexReflectCommandHandler.h"
#include "CortexCommandContract.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexReflectOps.h"
#include "Operations/CortexReflectDependencyGraphOps.h"
#include "Operations/CortexReflectNameLookupOps.h"
#include "Operations/CortexReflectDuplicateGraphOps.h"

namespace
{
TArray<FCortexCommandInfo> ReflectCommandInfos();
}

FCortexCommandResult FCortexReflectCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

	static const TArray<FCortexCommandInfo> Infos = ReflectCommandInfos();
	FCortexCommandResult ContractError;
	if (!FCortexCommandContract::ValidateDeclaredParams(Infos, Command, Params, ContractError))
	{
		return ContractError;
	}

	if (Command == TEXT("find_usages"))
	{
		return FCortexReflectOps::FindUsages(Params);
	}

	if (Command == TEXT("find_overrides"))
	{
		return FCortexReflectOps::FindOverrides(Params);
	}

	if (Command == TEXT("search"))
	{
		return FCortexReflectOps::Search(Params);
	}

	if (Command == TEXT("class_hierarchy"))
	{
		return FCortexReflectOps::ClassHierarchy(Params);
	}

	if (Command == TEXT("blueprint_catalog"))
	{
		return FCortexReflectOps::BlueprintCatalog(Params);
	}

	if (Command == TEXT("class_detail"))
	{
		return FCortexReflectOps::ClassDetail(Params);
	}

	if (Command == TEXT("get_dependencies"))
	{
		return FCortexReflectOps::GetDependencies(Params);
	}

	if (Command == TEXT("get_referencers"))
	{
		return FCortexReflectOps::GetReferencers(Params);
	}

	if (Command == TEXT("find_broken_config_paths"))
	{
		return FCortexReflectOps::FindBrokenConfigPaths(Params);
	}

	if (Command == TEXT("export_dependency_graph"))
	{
		return FCortexReflectDependencyGraphOps::ExportDependencyGraph(Params);
	}

	if (Command == TEXT("find_name_lookups"))
	{
		return FCortexReflectNameLookupOps::FindNameLookups(Params);
	}

	if (Command == TEXT("find_duplicate_graphs"))
	{
		return FCortexReflectDuplicateGraphOps::FindDuplicateGraphs(Params);
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown reflect command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexReflectCommandHandler::GetSupportedCommands() const
{
	return ReflectCommandInfos();
}

namespace
{
TArray<FCortexCommandInfo> ReflectCommandInfos()
{
	return {
		FCortexCommandInfo{ TEXT("class_hierarchy"), TEXT("Get class inheritance tree") }
			.Required(TEXT("root"), TEXT("string"), TEXT("Root class name or Blueprint asset path"))
			.Optional(TEXT("depth"), TEXT("number"), TEXT("Maximum inheritance depth to traverse"))
			.Optional(TEXT("include_blueprint"), TEXT("boolean"), TEXT("Include Blueprint-derived classes"))
			.Optional(TEXT("include_engine"), TEXT("boolean"), TEXT("Include engine classes"))
			.Optional(TEXT("include_flat_classes"), TEXT("boolean"), TEXT("Include the flat duplicate of every tree node name (default false)"))
			.Optional(TEXT("max_results"), TEXT("number"), TEXT("Maximum classes to return")),
		FCortexCommandInfo{ TEXT("blueprint_catalog"), TEXT("List saved project Blueprint classes derived from a native class") }
			.Required(TEXT("root"), TEXT("string"), TEXT("Native root class name or script class path")),
		FCortexCommandInfo{ TEXT("class_detail"), TEXT("Get detailed info for a single class") }
			.Required(TEXT("class_name"), TEXT("string"), TEXT("Class name or Blueprint asset path"))
			.Optional(TEXT("include_inherited"), TEXT("boolean"), TEXT("Include inherited members in the response"))
			.Optional(TEXT("detail"), TEXT("string"), TEXT("Detail level: 'summary' (identity/parent/routing only) or 'full' (default); any other value is refused")),
		FCortexCommandInfo{ TEXT("find_overrides"), TEXT("Find Blueprint overrides of a class") }
			.Required(TEXT("class_name"), TEXT("string"), TEXT("Base class to inspect for overrides"))
			.Optional(TEXT("depth"), TEXT("number"), TEXT("Inheritance depth to search"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum overrides to return")),
		FCortexCommandInfo{ TEXT("find_usages"), TEXT("Find cross-references to a symbol") }
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name to search for"))
			.Optional(TEXT("class_name"), TEXT("string"), TEXT("Restrict search to a class context"))
			.Optional(TEXT("scope"), TEXT("string"), TEXT("Search scope"))
			.Optional(TEXT("deep_scan"), TEXT("boolean"), TEXT("Scan Blueprint bytecode for references"))
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Restrict matches to a path prefix"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum usage results to return"))
			.Optional(TEXT("max_blueprints"), TEXT("number"), TEXT("Maximum Blueprints to inspect during deep scans")),
		FCortexCommandInfo{ TEXT("search"), TEXT("Search classes by pattern") }
			.Required(TEXT("pattern"), TEXT("string"), TEXT("Class-name pattern to search for"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum classes to return"))
			.Optional(TEXT("include_engine"), TEXT("boolean"), TEXT("Include engine classes in search results")),
		FCortexCommandInfo{ TEXT("get_dependencies"), TEXT("Get asset dependencies from Asset Registry") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to inspect"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Dependency category filter"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum dependencies to return")),
		FCortexCommandInfo{ TEXT("get_referencers"), TEXT("Get asset referencers from Asset Registry") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to inspect"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Referencer category filter"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum referencers to return; config rows are listed first and count toward the limit, so asset rows fill what remains"))
			.Optional(TEXT("include_config"), TEXT("boolean"), TEXT("Include config/INI references as type 'config' rows (default true)")),
		FCortexCommandInfo{ TEXT("find_broken_config_paths"), TEXT("Report content package and directory paths in project and project-plugin INI files that do not exist. Wildcard patterns (a path containing * or ?) and the Old* attributes of CoreRedirects entries are not references and are skipped. scanned_files counts the INI files actually read; unreadable_files lists the ones that could not be read") }
			.StrictParams(),
		FCortexCommandInfo{ TEXT("export_dependency_graph"), TEXT("Write a typed dependency graph (inherits, implements, calls_message, casts, spawns, creates_widget, widget_tree_child, listview_entry, level_placement, struct_use, enum_use, config) to a report file. Scheduled nodes are Blueprints, Worlds, user-defined structs and enums under the prefixes; targets outside the prefixes are external nodes. calls_message covers interface message nodes and typed calls whose function belongs to an interface class. level_placement means a World asset hard-depends on the Blueprint package, or an external actor package (One-File-Per-Actor / World Partition, /<Root>/__ExternalActors__/<map path>/...) of that World references the Blueprint. Self-referencing edges are dropped. Waves order strongly connected components by longest path using hard kinds by default; cycles lists components of more than one node. matched_assets reports the assets found under each path prefix and warnings names prefixes that matched nothing") }
			.Required(TEXT("path_prefixes"), TEXT("array"), TEXT("Mounted content folders whose assets form the graph (e.g. /Game/MyFeature)"))
			.Required(TEXT("out_path"), TEXT("string"), TEXT("Report file under the project Saved, Content or Config directory"))
			.Optional(TEXT("edge_kinds"), TEXT("array"), TEXT("Edge kinds to derive (default all): inherits, implements, calls_message, casts, spawns, creates_widget, widget_tree_child, listview_entry, level_placement, struct_use, enum_use, config"))
			.Optional(TEXT("ordering_kinds"), TEXT("array"), TEXT("Edge kinds that order waves (default: inherits, implements, struct_use, enum_use, widget_tree_child, listview_entry); other kinds are emitted as edges only"))
			.StrictParams(),
		FCortexCommandInfo{ TEXT("find_name_lookups"), TEXT("Find by-name access to Blueprint members. C++ (project and project-plugin .h/.inl/.cpp sources, fixed roots; the UnrealCortex plugin is excluded): string-literal names passed to FindFunction, FindFunctionByName, FindFunctionChecked, FindPropertyByName, FindFProperty, FindUField, FindField, FindFieldChecked, CreateUFunction, BindUFunction, AddUFunction, RemoveUFunction, K2_SetTimer, K2_ClearTimer, K2_PauseTimer, K2_UnPauseTimer, K2_IsTimerActive, K2_IsTimerPaused, K2_TimerExists, K2_GetTimerElapsedTime, K2_GetTimerRemainingTime, and the first token of a literal CallFunctionByNameWithArguments command. Blueprint: K2_SetTimer, K2_SetTimerForNextTick and the K2_Clear/Pause/UnPause/IsActive/IsPaused/Exists/Elapsed/Remaining timer nodes with an unlinked literal FunctionName pin. Only literal names are detected: names held in variables or constants are not. Each hit lists the Blueprint functions, events, custom events, variables and component variables of that name under the prefixes; matched_names/unmatched_names split the looked-up names by whether any Blueprint member matched. scanned_files counts the source files actually read and unreadable_files lists the ones that could not be read; matched_assets reports the Blueprints found under each prefix and warnings names prefixes that matched nothing. With out_path the full result is written to that file and the response is a compact summary (counts plus the first 10 cpp, blueprint, matched and unmatched entries)") }
			.Required(TEXT("path_prefixes"), TEXT("array"), TEXT("Mounted content folders whose Blueprints are matched against (e.g. /Game/MyFeature)"))
			.Optional(TEXT("symbols"), TEXT("array"), TEXT("Only report looked-up names in this list (case-insensitive)"))
			.Optional(TEXT("out_path"), TEXT("string"), TEXT("Write the full result to this file (under Project Saved, Content or Config) and return only a compact summary; use it when the inline result would be too large"))
			.StrictParams(),
		FCortexCommandInfo{ TEXT("find_duplicate_graphs"), TEXT("Find duplicated logic across Blueprints. Graph units are function graphs, macro graphs and ubergraph event chains (an event node plus everything reachable from it). Units are normalised ignoring GUIDs, node names, positions, comments, reroute knots and disabled nodes; references to the owning Blueprint's own class read as 'self'. A node is described by class, member reference, typed pins of entry/result/event nodes, reflected scalar properties (object, enum, name, number) and non-default unlinked pin values; collapsed-graph contents are not expanded and function local variables (their types and defaults) are not compared. exact_groups share an identical normalised hash across two or more Blueprints (hash by iterative neighbourhood refinement, not a full isomorphism test, so rare regular structures may collide). similar_groups are same-named units of different Blueprints whose LOOSE node-descriptor multiset Jaccard ratio reaches similarity (loose: every reference to a Blueprint-generated class other than the Blueprint itself - member parents, cast targets, pin types, class pin defaults, widget classes - is replaced by its nearest native ancestor class name, and BP member references keep the member name only, so parallel sibling Blueprint classes compare equal; marked loose:true; exact groups stay strict), grouped as star clusters around a representative; members identical to each other collapse to one, and min_similarity/max_similarity are true over all member pairs (members linked only through the representative may be below the threshold). Widget trees: exact groups compare strict class+name multisets; similarity and widget_tree_similarity use (widget name, native ancestor class, parent widget name) tuples (kind widget_tree). blueprint_pairs counts each unit pair once and lists Blueprint pairs sharing exact or similar graphs, sorted by shared_graphs descending, with widget_tree_similarity. matched_assets reports the Blueprints found under each prefix and warnings names prefixes that matched nothing. Pairwise work is O(n^2) per same-name bucket, pruned by a size-ratio bound. With out_path the full result is written to that file and the response is a compact summary (counts plus the first 10 exact groups, similar groups and Blueprint pairs)") }
			.Required(TEXT("path_prefixes"), TEXT("array"), TEXT("Mounted content folders whose Blueprints are compared (e.g. /Game/MyFeature)"))
			.Optional(TEXT("min_nodes"), TEXT("number"), TEXT("Smallest graph (or widget tree) size considered, integer greater than 0 (default 5)"))
			.Optional(TEXT("similarity"), TEXT("number"), TEXT("Jaccard threshold for similar groups, greater than 0 and at most 1 (default 0.9)"))
			.Optional(TEXT("out_path"), TEXT("string"), TEXT("Write the full result to this file (under Project Saved, Content or Config) and return only a compact summary; use it when the inline result would be too large"))
			.StrictParams(),
	};
}
}
