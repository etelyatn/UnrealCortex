#include "CortexReflectCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexReflectOps.h"

FCortexCommandResult FCortexReflectCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

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

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown reflect command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexReflectCommandHandler::GetSupportedCommands() const
{
	return {
		{ TEXT("class_hierarchy"), TEXT("Get class inheritance tree") },
		{ TEXT("class_detail"), TEXT("Get detailed info for a single class") },
		{ TEXT("find_overrides"), TEXT("Find Blueprint overrides of a class") },
		{ TEXT("find_usages"), TEXT("Find cross-references to a symbol") },
		{ TEXT("search"), TEXT("Search classes by pattern") },
	};
}
