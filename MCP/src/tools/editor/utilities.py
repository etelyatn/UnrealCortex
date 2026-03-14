"""Utility tools for the editor domain."""

import logging

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def register_editor_utility_tools(mcp, connection: UEConnection):
    """Register utility tools."""

    @mcp.tool()
    def get_editor_state() -> str:
        """Get editor project/map/PIE state.

        No caching: editor state is dynamic.
        """
        try:
            response = connection.send_command("editor.get_editor_state")
            return format_response(response.get("data", {}), "get_editor_state")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_recent_logs(
        severity: str = "log",
        since_seconds: float = 30.0,
        since_cursor: int = -1,
        category: str = "",
    ) -> str:
        """Read recent log entries with cursor support."""
        params = {
            "severity": severity,
            "since_seconds": since_seconds,
            "since_cursor": since_cursor,
            "category": category,
        }
        try:
            response = connection.send_command("editor.get_recent_logs", params)
            return format_response(response.get("data", {}), "get_recent_logs")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def execute_console_command(command: str) -> str:
        """Execute console command in PIE world."""
        try:
            response = connection.send_command("editor.execute_console_command", {"command": command})
            return format_response(response.get("data", {}), "execute_console_command")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_time_dilation(factor: float) -> str:
        """Set PIE time dilation factor."""
        try:
            response = connection.send_command("editor.set_time_dilation", {"factor": factor})
            return format_response(response.get("data", {}), "set_time_dilation")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_world_info() -> str:
        """Get PIE world settings and runtime metadata."""
        try:
            response = connection.send_command("editor.get_world_info")
            return format_response(response.get("data", {}), "get_world_info")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"
