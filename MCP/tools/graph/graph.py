"""MCP tools for Blueprint graph read and write operations."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_GRAPHS = 120  # 2 min - graphs change during editing


def register_graph_tools(mcp, connection: UEConnection):
    """Register all Blueprint graph-related MCP tools."""

    @mcp.tool()
    def graph_list_graphs(asset_path: str) -> str:
        """List all graphs in a Blueprint asset.

        Returns all graphs including EventGraph, ConstructionScript, and function graphs.

        Args:
            asset_path: Full asset path to the Blueprint (e.g., '/Game/Blueprints/BP_Player.BP_Player').

        Returns:
            JSON with 'graphs' array, each containing:
            - name: Graph name (e.g., 'EventGraph', 'ReceiveBeginPlay')
            - class: Graph class (e.g., 'UEdGraph')
            - node_count: Number of nodes in the graph
        """
        try:
            response = connection.send_command_cached("graph.list_graphs", {
                "asset_path": asset_path
            }, ttl=_TTL_GRAPHS)
            return format_response(response.get("data", {}), "list_graphs")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_list_nodes(asset_path: str, graph_name: str = "EventGraph") -> str:
        """List all nodes in a specific graph.

        Returns summary information for each node including its ID, class, and position.

        Args:
            asset_path: Full asset path to the Blueprint.
            graph_name: Name of the graph to query (default: 'EventGraph').

        Returns:
            JSON with 'nodes' array, each containing:
            - node_id: Unique identifier for the node
            - class: Node class name (e.g., 'UK2Node_Event')
            - display_name: Display name of the node
            - position: {x, y} coordinates
            - pin_count: Number of pins on the node
        """
        try:
            response = connection.send_command_cached("graph.list_nodes", {
                "asset_path": asset_path,
                "graph_name": graph_name
            }, ttl=_TTL_GRAPHS)
            return format_response(response.get("data", {}), "list_nodes")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_get_node(asset_path: str, node_id: str, graph_name: str = "EventGraph") -> str:
        """Get detailed information about a specific node.

        Returns full node data including all pins and connections.

        Args:
            asset_path: Full asset path to the Blueprint.
            node_id: Unique identifier of the node (from list_nodes).
            graph_name: Name of the graph containing the node (default: 'EventGraph').

        Returns:
            JSON with:
            - node_id: The node identifier
            - class: Node class name
            - display_name: Display name of the node
            - position: {x, y} coordinates
            - pins: Detailed array of all input/output pins
        """
        try:
            response = connection.send_command_cached("graph.get_node", {
                "asset_path": asset_path,
                "node_id": node_id,
                "graph_name": graph_name
            }, ttl=_TTL_GRAPHS)
            return format_response(response.get("data", {}), "get_node")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_add_node(
        asset_path: str,
        node_class: str,
        graph_name: str = "EventGraph",
        position: str = "",  # JSON string: {"x": <int>, "y": <int>}
        params: str = ""  # JSON string: node-specific parameters
    ) -> str:
        """Add a new node to a Blueprint graph.

        Creates a new node of the specified class at the given position with optional parameters.

        Args:
            asset_path: Full asset path to the Blueprint.
            node_class: Class name of the node to create (e.g., 'UK2Node_CallFunction').
            graph_name: Name of the graph to add to (default: 'EventGraph').
            position: Optional JSON string with {x, y} coordinates (e.g., '{"x": 100, "y": 200}').
            params: Optional JSON string with node-specific parameters (e.g., '{"FunctionName": "PrintString"}').

        Returns:
            JSON with:
            - node_id: Identifier of the newly created node
            - success: Whether the operation succeeded
        """
        try:
            request = {
                "asset_path": asset_path,
                "node_class": node_class,
                "graph_name": graph_name
            }
            if position:
                request["position"] = json.loads(position)
            if params:
                request["params"] = json.loads(params)

            response = connection.send_command("graph.add_node", request)
            connection.invalidate_cache("graph.")
            return format_response(response.get("data", {}), "add_node")
        except json.JSONDecodeError as e:
            return f"Error: Invalid JSON in position or params: {e}"
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_remove_node(asset_path: str, node_id: str, graph_name: str = "EventGraph") -> str:
        """Remove a node from a Blueprint graph.

        Deletes the specified node and all its connections.

        Args:
            asset_path: Full asset path to the Blueprint.
            node_id: Unique identifier of the node to remove.
            graph_name: Name of the graph containing the node (default: 'EventGraph').

        Returns:
            JSON with success status and the removed node_id.
        """
        try:
            response = connection.send_command("graph.remove_node", {
                "asset_path": asset_path,
                "node_id": node_id,
                "graph_name": graph_name
            })
            connection.invalidate_cache("graph.")
            return format_response(response.get("data", {}), "remove_node")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_connect(
        asset_path: str,
        source_node: str,
        source_pin: str,
        target_node: str,
        target_pin: str,
        graph_name: str = "EventGraph"
    ) -> str:
        """Connect two nodes in a Blueprint graph.

        Creates a connection from a source pin to a target pin.

        Args:
            asset_path: Full asset path to the Blueprint.
            source_node: Node ID of the source node.
            source_pin: Name of the output pin on the source node.
            target_node: Node ID of the target node.
            target_pin: Name of the input pin on the target node.
            graph_name: Name of the graph containing the nodes (default: 'EventGraph').

        Returns:
            JSON with success status and connection details.
        """
        try:
            response = connection.send_command("graph.connect", {
                "asset_path": asset_path,
                "source_node": source_node,
                "source_pin": source_pin,
                "target_node": target_node,
                "target_pin": target_pin,
                "graph_name": graph_name
            })
            connection.invalidate_cache("graph.")
            return format_response(response.get("data", {}), "connect")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_disconnect(
        asset_path: str,
        node_id: str,
        pin_name: str,
        graph_name: str = "EventGraph"
    ) -> str:
        """Disconnect a pin in a Blueprint graph.

        Removes all connections from the specified pin.

        Args:
            asset_path: Full asset path to the Blueprint.
            node_id: Node ID containing the pin.
            pin_name: Name of the pin to disconnect.
            graph_name: Name of the graph containing the node (default: 'EventGraph').

        Returns:
            JSON with success status and number of connections removed.
        """
        try:
            response = connection.send_command("graph.disconnect", {
                "asset_path": asset_path,
                "node_id": node_id,
                "pin_name": pin_name,
                "graph_name": graph_name
            })
            connection.invalidate_cache("graph.")
            return format_response(response.get("data", {}), "disconnect")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_set_pin_value(
        asset_path: str,
        node_id: str,
        pin_name: str,
        value: str,
        graph_name: str = "EventGraph"
    ) -> str:
        """Set the default value of an input pin in a Blueprint graph.

        Sets the default value for an input pin. The pin must not be connected.

        Args:
            asset_path: Full asset path to the Blueprint.
            node_id: Node ID containing the pin.
            pin_name: Name of the input pin to set.
            value: The value to set (as a string - will be converted to the appropriate type).
            graph_name: Name of the graph containing the node (default: 'EventGraph').

        Returns:
            JSON with success status and the set value.
        """
        try:
            response = connection.send_command("graph.set_pin_value", {
                "asset_path": asset_path,
                "node_id": node_id,
                "pin_name": pin_name,
                "value": value,
                "graph_name": graph_name
            })
            connection.invalidate_cache("graph.")
            return format_response(response.get("data", {}), "set_pin_value")
        except ConnectionError as e:
            return f"Error: {e}"
