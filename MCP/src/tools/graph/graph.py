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
    def graph_list_graphs(asset_path: str, include_subgraphs: bool = False) -> str:
        """List all graphs in a Blueprint asset.

        Returns all graphs including EventGraph, ConstructionScript, and function graphs.

        Workflow for composites: call with include_subgraphs=True to discover
        composite subgraphs, then use the returned subgraph_path with other tools.

        Args:
            asset_path: Full asset path to the Blueprint (e.g., '/Game/Blueprints/BP_Player.BP_Player').
            include_subgraphs: If True, also lists composite subgraph entries with
                parent_graph and subgraph_path fields for direct navigation.

        Returns:
            JSON with 'graphs' array, each containing:
            - name: Graph name (e.g., 'EventGraph', 'ReceiveBeginPlay')
            - class: Graph class (e.g., 'UEdGraph')
            - node_count: Number of nodes in the graph
            - parent_graph: (subgraphs only) Name of the parent graph
            - subgraph_path: (subgraphs only) Dot-path for use in other tools
        """
        try:
            params = {"asset_path": asset_path}
            if include_subgraphs:
                params["include_subgraphs"] = True

            response = connection.send_command_cached("graph.list_graphs", params, ttl=_TTL_GRAPHS)
            return format_response(response.get("data", {}), "list_graphs")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_list_nodes(
        asset_path: str,
        graph_name: str = "EventGraph",
        subgraph_path: str = "",
        compact: bool = True,
    ) -> str:
        """List all nodes in a specific graph.

        Returns summary information for each node including its ID, class, and position.
        Composite nodes (UK2Node_Composite) include a 'subgraph_name' field with the
        name of the embedded subgraph, usable as a segment in subgraph_path.
        Tunnel boundary nodes inside composites have 'is_tunnel_boundary: true' --
        these are structural and must not be deleted or rewired.

        Composite nodes without 'subgraph_name' have no traversable subgraph
        (empty or pending compilation).

        Args:
            asset_path: Full asset path to the Blueprint.
            graph_name: Name of the graph to query (default: 'EventGraph').
            subgraph_path: Dot-separated path into nested composite subgraphs
                (e.g., 'BeginPlay.Inner'). Resolves from graph_name downward.
                To navigate into a composite, take the 'subgraph_name' value from a
                list_nodes result and pass it as subgraph_path on the next call.
                For deeper nesting, append further subgraph_name values with a dot:
                e.g. 'OuterComposite.InnerComposite'.
                Note: composite names must not contain dots.
            compact: Omit position, node_class, and pin_count fields (default: true).
                Set false to include positional data and redundant fields.

        Returns:
            JSON with 'nodes' array, each containing:
            - node_id: Unique identifier for the node
            - class: Node class name (e.g., 'UK2Node_Event')
            - display_name: Display name of the node
            - connected_pin_count: Number of pins with at least one connection
            - subgraph_name: (composite nodes only) Name of the embedded subgraph
            - is_tunnel_boundary: (tunnel nodes only) True for structural entry/exit nodes
            - position: {x, y} coordinates (compact=false only)
            - pin_count: Total number of pins (compact=false only)
        """
        try:
            params = {
                "asset_path": asset_path,
                "graph_name": graph_name,
                "compact": compact,
            }
            if subgraph_path:
                params["subgraph_path"] = subgraph_path

            response = connection.send_command_cached("graph.list_nodes", params, ttl=_TTL_GRAPHS)
            return format_response(response.get("data", {}), "list_nodes")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_get_node(
        asset_path: str,
        node_id: str,
        graph_name: str = "EventGraph",
        subgraph_path: str = "",
        compact: bool = True,
    ) -> str:
        """Get detailed information about a specific node.

        Returns full node data including all pins and connections.

        Args:
            asset_path: Full asset path to the Blueprint.
            node_id: Unique identifier of the node (from list_nodes).
            graph_name: Name of the graph containing the node (default: 'EventGraph').
            subgraph_path: Dot-separated path into nested composite subgraphs
                (e.g., 'BeginPlay.Inner'). Resolves from graph_name downward.
            compact: Omit position, node_class, and hidden unconnected pins (default: true).
                Set false to see all pins and positional data.

        Returns:
            JSON with node details and pins with connection info.
            If the node is a tunnel boundary (is_tunnel_boundary: true), its pins
            represent the composite's entry/exit execution and data pins. Inspect
            these before connecting nodes inside a composite to the execution flow.
        """
        try:
            params = {
                "asset_path": asset_path,
                "node_id": node_id,
                "graph_name": graph_name,
                "compact": compact,
            }
            if subgraph_path:
                params["subgraph_path"] = subgraph_path

            response = connection.send_command_cached("graph.get_node", params, ttl=_TTL_GRAPHS)
            return format_response(response.get("data", {}), "get_node")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_search_nodes(
        asset_path: str,
        node_class: str = "",
        function_name: str = "",
        display_name: str = "",
        graph_name: str = "",
        subgraph_path: str = "",
        compact: bool = True,
    ) -> str:
        """Search nodes across all Blueprint graphs.

        Finds nodes server-side using one or more filters. By default, recursively
        searches inside composite subgraphs too. Results include 'subgraph_path'
        for nodes found inside composites so you can navigate to them directly.

        Args:
            asset_path: Full asset path to the Blueprint.
            node_class: Optional node class filter (exact match).
            function_name: Optional partial/case-insensitive function-name filter.
            display_name: Optional partial/case-insensitive node display name filter.
            graph_name: Optional graph to restrict search to.
            subgraph_path: Dot-separated path to restrict search to a specific subgraph.
                Requires graph_name to be set.
            compact: Omit redundant node_class field from results (default: true).

        Returns:
            JSON with results array. Each entry includes graph_name and
            optionally subgraph_path for composite-nested nodes.
        """
        if not node_class and not function_name and not display_name:
            return "Error: At least one filter required: node_class, function_name, or display_name"
        if subgraph_path and not graph_name:
            return "Error: subgraph_path requires graph_name to be set"

        try:
            params = {"asset_path": asset_path, "compact": compact}
            if node_class:
                params["node_class"] = node_class
            if function_name:
                params["function_name"] = function_name
            if display_name:
                params["display_name"] = display_name
            if graph_name:
                params["graph_name"] = graph_name
            if subgraph_path:
                params["subgraph_path"] = subgraph_path

            response = connection.send_command_cached("graph.search_nodes", params, ttl=_TTL_GRAPHS)
            return format_response(response.get("data", {}), "search_nodes")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_add_node(
        asset_path: str,
        node_class: str,
        graph_name: str = "EventGraph",
        subgraph_path: str = "",
        position: str = "",
        params: str = "",
    ) -> str:
        """Add a new node to a Blueprint graph.

        Creates a new node of the specified class at the given position with optional parameters.

        Args:
            asset_path: Full asset path to the Blueprint.
            node_class: Class name of the node to create.
            graph_name: Name of the graph to add to (default: 'EventGraph').
            subgraph_path: Dot-separated path into nested composite subgraphs.
                Discover valid paths via graph_list_graphs(include_subgraphs=True)
                or by reading subgraph_name fields from graph_list_nodes output.
            position: Optional JSON string with {x, y} coordinates.
            params: Optional JSON string with node-specific parameters.

        Returns:
            JSON with node_id, class, display_name, and pins.
        """
        try:
            request = {
                "asset_path": asset_path,
                "node_class": node_class,
                "graph_name": graph_name,
            }
            subgraph_path = subgraph_path.strip()
            if subgraph_path:
                request["subgraph_path"] = subgraph_path
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
    def graph_remove_node(
        asset_path: str,
        node_id: str,
        graph_name: str = "EventGraph",
        subgraph_path: str = "",
    ) -> str:
        """Remove a node from a Blueprint graph.

        Deletes the specified node and all its connections.

        Args:
            asset_path: Full asset path to the Blueprint.
            node_id: Unique identifier of the node to remove.
            graph_name: Name of the graph containing the node (default: 'EventGraph').
            subgraph_path: Dot-separated path into nested composite subgraphs.
                Discover valid paths via graph_list_graphs(include_subgraphs=True)
                or by reading subgraph_name fields from graph_list_nodes output.

        Returns:
            JSON with success status and the removed node_id.
        """
        try:
            request = {
                "asset_path": asset_path,
                "node_id": node_id,
                "graph_name": graph_name,
            }
            subgraph_path = subgraph_path.strip()
            if subgraph_path:
                request["subgraph_path"] = subgraph_path

            response = connection.send_command("graph.remove_node", request)
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
        graph_name: str = "EventGraph",
        subgraph_path: str = "",
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
            subgraph_path: Dot-separated path into nested composite subgraphs.
                Discover valid paths via graph_list_graphs(include_subgraphs=True)
                or by reading subgraph_name fields from graph_list_nodes output.

        Returns:
            JSON with success status and connection details.
        """
        try:
            request = {
                "asset_path": asset_path,
                "source_node": source_node,
                "source_pin": source_pin,
                "target_node": target_node,
                "target_pin": target_pin,
                "graph_name": graph_name,
            }
            subgraph_path = subgraph_path.strip()
            if subgraph_path:
                request["subgraph_path"] = subgraph_path

            response = connection.send_command("graph.connect", request)
            connection.invalidate_cache("graph.")
            return format_response(response.get("data", {}), "connect")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_disconnect(
        asset_path: str,
        node_id: str,
        pin_name: str,
        graph_name: str = "EventGraph",
        subgraph_path: str = "",
    ) -> str:
        """Disconnect a pin in a Blueprint graph.

        Removes all connections from the specified pin.

        Args:
            asset_path: Full asset path to the Blueprint.
            node_id: Node ID containing the pin.
            pin_name: Name of the pin to disconnect.
            graph_name: Name of the graph containing the node (default: 'EventGraph').
            subgraph_path: Dot-separated path into nested composite subgraphs.
                Discover valid paths via graph_list_graphs(include_subgraphs=True)
                or by reading subgraph_name fields from graph_list_nodes output.

        Returns:
            JSON with success status and number of connections removed.
        """
        try:
            request = {
                "asset_path": asset_path,
                "node_id": node_id,
                "pin_name": pin_name,
                "graph_name": graph_name,
            }
            subgraph_path = subgraph_path.strip()
            if subgraph_path:
                request["subgraph_path"] = subgraph_path

            response = connection.send_command("graph.disconnect", request)
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
        graph_name: str = "EventGraph",
        subgraph_path: str = "",
    ) -> str:
        """Set the default value of an input pin in a Blueprint graph.

        Sets the default value for an input pin. The pin must not be connected.

        Args:
            asset_path: Full asset path to the Blueprint.
            node_id: Node ID containing the pin.
            pin_name: Name of the input pin to set.
            value: The value to set (as a string - will be converted to the appropriate type).
            graph_name: Name of the graph containing the node (default: 'EventGraph').
            subgraph_path: Dot-separated path into nested composite subgraphs.
                Discover valid paths via graph_list_graphs(include_subgraphs=True)
                or by reading subgraph_name fields from graph_list_nodes output.

        Returns:
            JSON with success status and the set value.
        """
        try:
            request = {
                "asset_path": asset_path,
                "node_id": node_id,
                "pin_name": pin_name,
                "value": value,
                "graph_name": graph_name,
            }
            subgraph_path = subgraph_path.strip()
            if subgraph_path:
                request["subgraph_path"] = subgraph_path

            response = connection.send_command("graph.set_pin_value", request)
            connection.invalidate_cache("graph.")
            return format_response(response.get("data", {}), "set_pin_value")
        except ConnectionError as e:
            return f"Error: {e}"
