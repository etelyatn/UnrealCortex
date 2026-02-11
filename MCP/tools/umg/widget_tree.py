"""MCP tools for UMG widget tree operations."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_LIST = 300


def register_widget_tree_tools(mcp, connection: UEConnection):
    """Register all widget tree MCP tools."""

    @mcp.tool()
    def add_widget(
        asset_path: str,
        widget_class: str,
        name: str,
        parent_name: str = "",
        slot_index: int = -1,
    ) -> str:
        """Add a widget to a Widget Blueprint's widget tree."""
        params = {"asset_path": asset_path, "widget_class": widget_class, "name": name}
        if parent_name:
            params["parent_name"] = parent_name
        if slot_index >= 0:
            params["slot_index"] = slot_index
        try:
            response = connection.send_command("umg.add_widget", params)
            return format_response(response.get("data", {}), "add_widget")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def remove_widget(asset_path: str, widget_name: str) -> str:
        """Remove a widget and its subtree from a Widget Blueprint."""
        try:
            response = connection.send_command(
                "umg.remove_widget",
                {"asset_path": asset_path, "widget_name": widget_name},
            )
            return format_response(response.get("data", {}), "remove_widget")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def reparent(
        asset_path: str, widget_name: str, new_parent: str, slot_index: int = -1
    ) -> str:
        """Move a widget to a different parent in the widget tree."""
        params = {
            "asset_path": asset_path,
            "widget_name": widget_name,
            "new_parent": new_parent,
        }
        if slot_index >= 0:
            params["slot_index"] = slot_index
        try:
            response = connection.send_command("umg.reparent", params)
            return format_response(response.get("data", {}), "reparent")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_tree(asset_path: str) -> str:
        """Get the full widget hierarchy of a Widget Blueprint."""
        try:
            response = connection.send_command_cached(
                "umg.get_tree", {"asset_path": asset_path}, ttl=_TTL_LIST
            )
            return format_response(response.get("data", {}), "get_tree")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_widget(asset_path: str, widget_name: str) -> str:
        """Get detailed information about a single widget."""
        try:
            response = connection.send_command(
                "umg.get_widget",
                {"asset_path": asset_path, "widget_name": widget_name},
            )
            return format_response(response.get("data", {}), "get_widget")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def list_widget_classes(category: str = "") -> str:
        """List available UMG widget classes."""
        try:
            response = connection.send_command_cached(
                "umg.list_widget_classes",
                {"category": category} if category else {},
                ttl=_TTL_LIST,
            )
            return format_response(response.get("data", {}), "list_widget_classes")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def duplicate_widget(
        asset_path: str,
        widget_name: str,
        new_name: str,
        name_prefix: str = "",
    ) -> str:
        """Duplicate a widget and its subtree with new names."""
        params = {
            "asset_path": asset_path,
            "widget_name": widget_name,
            "new_name": new_name,
        }
        if name_prefix:
            params["name_prefix"] = name_prefix
        try:
            response = connection.send_command("umg.duplicate_widget", params)
            return format_response(response.get("data", {}), "duplicate_widget")
        except ConnectionError as e:
            return f"Error: {e}"
