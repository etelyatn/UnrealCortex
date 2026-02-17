"""MCP tools for level streaming, world partition, and persistence operations."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_level_streaming_tools(mcp, connection: UEConnection):
    """Register streaming/WP/persistence MCP tools."""

    @mcp.tool()
    def get_info() -> str:
        try:
            response = connection.send_command("level.get_info", {})
            return format_response(response.get("data", {}), "get_info")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def list_sublevels() -> str:
        try:
            response = connection.send_command("level.list_sublevels", {})
            return format_response(response.get("data", {}), "list_sublevels")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def load_sublevel(level: str) -> str:
        try:
            response = connection.send_command("level.load_sublevel", {"level": level})
            return format_response(response.get("data", {}), "load_sublevel")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def unload_sublevel(level: str) -> str:
        try:
            response = connection.send_command("level.unload_sublevel", {"level": level})
            return format_response(response.get("data", {}), "unload_sublevel")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_sublevel_visibility(level: str, visible: bool) -> str:
        try:
            response = connection.send_command(
                "level.set_sublevel_visibility", {"level": level, "visible": visible}
            )
            return format_response(response.get("data", {}), "set_sublevel_visibility")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def list_data_layers() -> str:
        try:
            response = connection.send_command("level.list_data_layers", {})
            return format_response(response.get("data", {}), "list_data_layers")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_data_layer(actor: str, data_layer: str) -> str:
        try:
            response = connection.send_command(
                "level.set_data_layer", {"actor": actor, "data_layer": data_layer}
            )
            return format_response(response.get("data", {}), "set_data_layer")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def save_level() -> str:
        try:
            response = connection.send_command("level.save_level", {})
            return format_response(response.get("data", {}), "save_level")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def save_all() -> str:
        try:
            response = connection.send_command("level.save_all", {})
            return format_response(response.get("data", {}), "save_all")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
