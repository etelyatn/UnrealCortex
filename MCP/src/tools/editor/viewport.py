"""Viewport tools for the editor domain."""

import logging

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def register_editor_viewport_tools(mcp, connection: UEConnection):
    """Register viewport tools."""

    @mcp.tool()
    def get_viewport_info() -> str:
        """Get viewport resolution, camera, and mode.

        No caching: viewport state changes continuously.
        """
        try:
            response = connection.send_command("editor.get_viewport_info")
            return format_response(response.get("data", {}), "get_viewport_info")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def capture_screenshot(output_path: str = "") -> str:
        """Capture a PNG screenshot from the active editor viewport."""
        params = {}
        if output_path:
            params["output_path"] = output_path
        try:
            response = connection.send_command("editor.capture_screenshot", params, timeout=60.0)
            return format_response(response.get("data", {}), "capture_screenshot")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_viewport_camera(
        x: float,
        y: float,
        z: float,
        pitch: float = 0.0,
        yaw: float = 0.0,
        roll: float = 0.0,
    ) -> str:
        """Set editor viewport camera location and rotation."""
        params = {
            "location": {"x": x, "y": y, "z": z},
            "rotation": {"pitch": pitch, "yaw": yaw, "roll": roll},
        }
        try:
            response = connection.send_command("editor.set_viewport_camera", params)
            return format_response(response.get("data", {}), "set_viewport_camera")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def focus_actor(actor_path: str) -> str:
        """Frame actor in active viewport by object path."""
        try:
            response = connection.send_command("editor.focus_actor", {"actor_path": actor_path})
            return format_response(response.get("data", {}), "focus_actor")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def focus_node(asset_path: str, node_id: str, graph_name: str = "") -> str:
        """Open a Blueprint in the editor and focus a specific graph node.

        Opens the Blueprint asset editor, navigates to the graph containing the node,
        and centers the viewport on it with selection. Use after graph_list_nodes or
        graph_get_node when you need to direct the user's attention to a specific node
        -- for example, a node with incorrect pin values, a broken connection, or a
        compilation error source.

        This is a UI navigation tool, not a data tool:
        - To READ node data without opening the editor, use graph_get_node.
        - To OPEN a Blueprint without focusing a specific node, use open_asset.
        - To FOCUS a level actor in the 3D viewport, use focus_actor.

        Args:
            asset_path: Full asset path to the Blueprint
                        (e.g., '/Game/UI/WBP_UpgradeItem.WBP_UpgradeItem').
            node_id: The node ID to focus (e.g., 'K2Node_FormatText_0').
                     Get IDs from graph_list_nodes.
            graph_name: Optional graph name (e.g., 'UpdateLocked').
                        If omitted, searches all graphs for the node.

        Returns:
            JSON with focused node details:
            - asset_path: The Blueprint asset path
            - graph_name: The graph containing the node (useful when graph_name was omitted)
            - node_id: The focused node ID
            - display_name: Human-readable node title
            - node_class: The node UClass name
        """
        params = {"asset_path": asset_path, "node_id": node_id}
        if graph_name:
            params["graph_name"] = graph_name
        try:
            response = connection.send_command("editor.focus_node", params)
            return format_response(response.get("data", {}), "focus_node")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_viewport_mode(mode: str = "lit") -> str:
        """Set viewport rendering mode (lit, unlit, wireframe, lit_wireframe)."""
        try:
            response = connection.send_command("editor.set_viewport_mode", {"mode": mode})
            return format_response(response.get("data", {}), "set_viewport_mode")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"
