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
    def set_viewport_mode(mode: str = "lit") -> str:
        """Set viewport rendering mode (lit, unlit, wireframe, lit_wireframe)."""
        try:
            response = connection.send_command("editor.set_viewport_mode", {"mode": mode})
            return format_response(response.get("data", {}), "set_viewport_mode")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"
