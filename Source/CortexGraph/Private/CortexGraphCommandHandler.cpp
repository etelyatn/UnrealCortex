#include "CortexGraphCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Operations/CortexGraphConnectionOps.h"

FCortexCommandResult FCortexGraphCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

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
	if (Command == TEXT("search_nodes"))
	{
		return FCortexGraphNodeOps::SearchNodes(Params);
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
	if (Command == TEXT("set_pin_value"))
	{
		return FCortexGraphNodeOps::SetPinValue(Params);
	}
	if (Command == TEXT("auto_layout"))
	{
		return FCortexGraphNodeOps::AutoLayout(Params);
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown graph command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexGraphCommandHandler::GetSupportedCommands() const
{
	return {
		FCortexCommandInfo{ TEXT("list_graphs"), TEXT("List all graphs in an asset") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Optional(TEXT("include_subgraphs"), TEXT("boolean"), TEXT("Include composite subgraphs with parent_graph and subgraph_path fields")),
		FCortexCommandInfo{ TEXT("list_nodes"), TEXT("List nodes in a graph") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph to inspect, defaults to EventGraph"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')"))
			.Optional(TEXT("compact"), TEXT("boolean"), TEXT("Omit position, node_class, pin_count to reduce token usage (default: true)")),
		FCortexCommandInfo{ TEXT("get_node"), TEXT("Get node details with all pins") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Identifier of the node to inspect"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing the node"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')"))
			.Optional(TEXT("compact"), TEXT("boolean"), TEXT("Omit position, node_class; filter hidden unconnected pins (default: true)")),
		FCortexCommandInfo{ TEXT("search_nodes"), TEXT("Search nodes across graphs by class, function name, or display name") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Optional(TEXT("node_class"), TEXT("string"), TEXT("Runtime node class filter"))
			.Optional(TEXT("function_name"), TEXT("string"), TEXT("Function-name filter for call nodes"))
			.Optional(TEXT("display_name"), TEXT("string"), TEXT("Node display-name filter"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Restrict search to a specific graph"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path to restrict search"))
			.Optional(TEXT("compact"), TEXT("boolean"), TEXT("Omit node_class from results (default: true)")),
		FCortexCommandInfo{ TEXT("add_node"), TEXT("Add a node to a graph") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("node_class"), TEXT("string"), TEXT("Node class to create"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Target graph, defaults to EventGraph"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')"))
			.Optional(TEXT("position"), TEXT("object"), TEXT("Optional node placement coordinates"))
			.Optional(TEXT("params"), TEXT("object"), TEXT("Node-specific creation parameters")),
		FCortexCommandInfo{ TEXT("remove_node"), TEXT("Remove a node and clean up connections") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Identifier of the node to remove"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing the node"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')")),
		FCortexCommandInfo{ TEXT("connect"), TEXT("Connect two pins") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("source_node"), TEXT("string"), TEXT("Node ID of the output node"))
			.Required(TEXT("source_pin"), TEXT("string"), TEXT("Output pin name"))
			.Required(TEXT("target_node"), TEXT("string"), TEXT("Node ID of the input node"))
			.Required(TEXT("target_pin"), TEXT("string"), TEXT("Input pin name"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing both nodes"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')")),
		FCortexCommandInfo{ TEXT("disconnect"), TEXT("Disconnect a pin") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node containing the pin"))
			.Required(TEXT("pin_name"), TEXT("string"), TEXT("Pin to disconnect"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing the node"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')")),
		FCortexCommandInfo{ TEXT("set_pin_value"), TEXT("Set the default value of an input pin") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node containing the pin"))
			.Required(TEXT("pin_name"), TEXT("string"), TEXT("Input pin to modify"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Serialized pin value"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing the node"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')")),
		FCortexCommandInfo{ TEXT("auto_layout"), TEXT("Auto-arrange nodes in Blueprint graphs for readability") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path to the Blueprint asset"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Specific graph to layout"))
			.Optional(TEXT("subgraph_path"), TEXT("string"), TEXT("Dot-separated composite subgraph path (e.g. 'BeginPlay.Inner')")),
	};
}
