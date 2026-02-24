#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPClassDefaultsOps.h"
#include "Operations/CortexBPComponentOps.h"
#include "Operations/CortexBPStructureOps.h"
#include "Operations/CortexBPTimelineOps.h"

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

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown bp command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexBPCommandHandler::GetSupportedCommands() const
{
	TArray<FCortexCommandInfo> Commands;

	Commands.Add({TEXT("create"), TEXT("Create a new Blueprint asset")});
	Commands.Add({TEXT("list"), TEXT("List Blueprint assets")});
	Commands.Add({TEXT("get_info"), TEXT("Get Blueprint info")});
	Commands.Add({TEXT("delete"), TEXT("Delete a Blueprint asset")});
	Commands.Add({TEXT("duplicate"), TEXT("Duplicate a Blueprint asset")});
	Commands.Add({TEXT("compile"), TEXT("Compile a Blueprint")});
	Commands.Add({TEXT("save"), TEXT("Save a Blueprint")});
	Commands.Add({TEXT("add_variable"), TEXT("Add a variable to a Blueprint")});
	Commands.Add({TEXT("remove_variable"), TEXT("Remove a variable from a Blueprint")});
	Commands.Add({TEXT("add_function"), TEXT("Add a function to a Blueprint")});
	Commands.Add({TEXT("get_class_defaults"), TEXT("Read default property values from a Blueprint CDO")});
	Commands.Add({TEXT("set_class_defaults"), TEXT("Set default property values on a Blueprint CDO")});
	Commands.Add({TEXT("configure_timeline"), TEXT("Configure a Timeline node's tracks and keyframes")});
	Commands.Add({TEXT("set_component_defaults"), TEXT("Set object-reference properties on a Blueprint component template")});

	return Commands;
}
