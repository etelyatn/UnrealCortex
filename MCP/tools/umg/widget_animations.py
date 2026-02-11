"""MCP tools for UMG widget animation operations."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_LIST = 300


def register_widget_animation_tools(mcp, connection: UEConnection):
    """Register all widget animation MCP tools."""

    @mcp.tool()
    def create_animation(asset_path: str, animation_name: str, length: float = 1.0) -> str:
        """Create a new UWidgetAnimation in a Widget Blueprint."""
        try:
            response = connection.send_command("umg.create_animation", {
                "asset_path": asset_path,
                "animation_name": animation_name,
                "length": length,
            })
            return format_response(response.get("data", {}), "create_animation")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def list_animations(asset_path: str) -> str:
        """List all animations in a Widget Blueprint."""
        try:
            response = connection.send_command_cached(
                "umg.list_animations", {"asset_path": asset_path}, ttl=_TTL_LIST
            )
            return format_response(response.get("data", {}), "list_animations")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def add_track(
        asset_path: str, animation_name: str, widget_name: str, property_path: str
    ) -> str:
        """Add a property animation track to an animation."""
        try:
            response = connection.send_command("umg.add_track", {
                "asset_path": asset_path,
                "animation_name": animation_name,
                "widget_name": widget_name,
                "property_path": property_path,
            })
            return format_response(response.get("data", {}), "add_track")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def add_keyframe(
        asset_path: str, animation_name: str, track_index: int,
        time: float, value: str, interp: str = "Linear",
    ) -> str:
        """Add a keyframe to an animation track."""
        params = {
            "asset_path": asset_path,
            "animation_name": animation_name,
            "track_index": track_index,
            "time": time,
            "interp": interp,
        }
        try:
            params["value"] = json.loads(value)
        except (json.JSONDecodeError, TypeError):
            params["value"] = value
        try:
            response = connection.send_command("umg.add_keyframe", params)
            return format_response(response.get("data", {}), "add_keyframe")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def remove_animation(asset_path: str, animation_name: str) -> str:
        """Remove an animation from the Widget Blueprint."""
        try:
            response = connection.send_command("umg.remove_animation", {
                "asset_path": asset_path,
                "animation_name": animation_name,
            })
            return format_response(response.get("data", {}), "remove_animation")
        except ConnectionError as e:
            return f"Error: {e}"
