"""MCP tools for graph trace and subgraph reads."""

import logging

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)

_TTL_GRAPHS = 120


def register_graph_trace_tools(mcp, connection: UEConnection):
    """Register graph trace/read MCP tools."""

    @mcp.tool()
    def graph_trace_exec(
        asset_path: str,
        start_node_id: str,
        max_depth: int = 10,
        traverse_policy: str = "Opaque",
        include_edges: bool = True,
        graph_name: str = "",
        subgraph_path: str = "",
    ) -> str:
        """Trace execution flow from a starting node."""
        try:
            params = {
                "asset_path": asset_path,
                "start_node_id": start_node_id,
                "max_depth": max_depth,
                "traverse_policy": traverse_policy,
                "include_edges": include_edges,
            }
            if graph_name:
                params["graph_name"] = graph_name
            if subgraph_path:
                params["subgraph_path"] = subgraph_path

            response = connection.send_command_cached("graph.trace_exec", params, ttl=_TTL_GRAPHS)
            return format_response(response.get("data", {}), "trace_exec")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_trace_dataflow(
        asset_path: str,
        start_node_id: str,
        max_depth: int = 10,
        traverse_policy: str = "Opaque",
        include_edges: bool = True,
        graph_name: str = "",
        subgraph_path: str = "",
    ) -> str:
        """Trace data-flow from a starting node."""
        try:
            params = {
                "asset_path": asset_path,
                "start_node_id": start_node_id,
                "max_depth": max_depth,
                "traverse_policy": traverse_policy,
                "include_edges": include_edges,
            }
            if graph_name:
                params["graph_name"] = graph_name
            if subgraph_path:
                params["subgraph_path"] = subgraph_path

            response = connection.send_command_cached("graph.trace_dataflow", params, ttl=_TTL_GRAPHS)
            return format_response(response.get("data", {}), "trace_dataflow")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_get_subgraph(
        asset_path: str,
        graph_name: str = "EventGraph",
        node_ids: list[str] | None = None,
        include_edges: bool = False,
        subgraph_path: str = "",
    ) -> str:
        """Read a resolved graph or a selected node subset."""
        try:
            params = {
                "asset_path": asset_path,
                "graph_name": graph_name,
                "include_edges": include_edges,
            }
            if node_ids:
                params["node_ids"] = node_ids
            if subgraph_path:
                params["subgraph_path"] = subgraph_path

            response = connection.send_command_cached("graph.get_subgraph", params, ttl=_TTL_GRAPHS)
            return format_response(response.get("data", {}), "get_subgraph")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_find_event_handler(asset_path: str, event_name: str) -> str:
        """Find event entry nodes by display name or identifier."""
        try:
            response = connection.send_command_cached(
                "graph.find_event_handler",
                {"asset_path": asset_path, "event_name": event_name},
                ttl=_TTL_GRAPHS,
            )
            return format_response(response.get("data", {}), "find_event_handler")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def graph_find_function_calls(asset_path: str, function_name: str) -> str:
        """Find call-function nodes by function name."""
        try:
            response = connection.send_command_cached(
                "graph.find_function_calls",
                {"asset_path": asset_path, "function_name": function_name},
                ttl=_TTL_GRAPHS,
            )
            return format_response(response.get("data", {}), "find_function_calls")
        except ConnectionError as e:
            return f"Error: {e}"
