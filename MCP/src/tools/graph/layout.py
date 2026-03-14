"""Graph layout tools — shared infrastructure for Blueprint and Material graphs."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def register_graph_layout_tools(mcp, connection: UEConnection):
    """Register graph layout MCP tools."""

    @mcp.tool()
    def graph_auto_layout(
        asset_path: str,
        mode: str = "full",
        graph_name: str | None = None,
        horizontal_spacing: int | None = None,
        vertical_spacing: int | None = None,
    ) -> str:
        """Auto-arrange nodes in Blueprint graphs for readability.

        Repositions nodes using execution-first left-to-right layout.
        Works on all graph types: EventGraph, function graphs, Widget BP graphs.

        Note: Nodes at position (0,0) in incremental mode are treated as unpositioned
        and will be repositioned. If a node was intentionally placed at origin, use
        full mode instead.

        Args:
            asset_path: Blueprint asset path (e.g., /Game/Blueprints/BP_Example)
            mode: "full" (reposition all nodes) or "incremental" (only new nodes at 0,0)
            graph_name: Specific graph to layout (default: all graphs)
            horizontal_spacing: Override horizontal gap between columns (default: 80)
            vertical_spacing: Override vertical gap between nodes (default: 40)
        """
        params = {"asset_path": asset_path, "mode": mode}
        if graph_name:
            params["graph_name"] = graph_name
        if horizontal_spacing is not None:
            params["horizontal_spacing"] = horizontal_spacing
        if vertical_spacing is not None:
            params["vertical_spacing"] = vertical_spacing
        response = connection.send_command("graph.auto_layout", params)
        connection.invalidate_cache("graph.")
        connection.invalidate_cache("bp.")
        return json.dumps(response.get("data", {}), indent=2)
