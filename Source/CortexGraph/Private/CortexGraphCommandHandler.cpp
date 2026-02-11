#include "CortexGraphCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Operations/CortexGraphConnectionOps.h"

FCortexCommandResult FCortexGraphCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params)
{
	if (Command == TEXT("list_graphs"))
	{
		return FCortexGraphNodeOps::ListGraphs(Params);
	}
	if (Command == TEXT("list_nodes"))
	{
		return FCortexGraphNodeOps::ListNodes(Params);
	}
	if (Command == TEXT("get_node"))
	{
		return FCortexGraphNodeOps::GetNode(Params);
	}
	if (Command == TEXT("add_node"))
	{
		return FCortexGraphNodeOps::AddNode(Params);
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

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown graph command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexGraphCommandHandler::GetSupportedCommands() const
{
	return {
		{ TEXT("list_graphs"), TEXT("List all graphs in an asset") },
		{ TEXT("list_nodes"), TEXT("List nodes in a graph") },
		{ TEXT("get_node"), TEXT("Get node details with all pins") },
		{ TEXT("add_node"), TEXT("Add a node to a graph") },
		{ TEXT("remove_node"), TEXT("Remove a node and clean up connections") },
		{ TEXT("connect"), TEXT("Connect two pins") },
		{ TEXT("disconnect"), TEXT("Disconnect a pin") },
	};
}
