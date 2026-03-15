#include "CortexReflectCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexReflectOps.h"

FCortexCommandResult FCortexReflectCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

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

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown reflect command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexReflectCommandHandler::GetSupportedCommands() const
{
	return {
		FCortexCommandInfo{ TEXT("class_hierarchy"), TEXT("Get class inheritance tree") }
			.Required(TEXT("root"), TEXT("string"), TEXT("Root class name or Blueprint asset path"))
			.Optional(TEXT("depth"), TEXT("number"), TEXT("Maximum inheritance depth to traverse"))
			.Optional(TEXT("include_blueprint"), TEXT("boolean"), TEXT("Include Blueprint-derived classes"))
			.Optional(TEXT("include_engine"), TEXT("boolean"), TEXT("Include engine classes"))
			.Optional(TEXT("max_results"), TEXT("number"), TEXT("Maximum classes to return")),
		FCortexCommandInfo{ TEXT("class_detail"), TEXT("Get detailed info for a single class") }
			.Required(TEXT("class_name"), TEXT("string"), TEXT("Class name or Blueprint asset path"))
			.Optional(TEXT("include_inherited"), TEXT("boolean"), TEXT("Include inherited members in the response")),
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
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum referencers to return")),
	};
}
