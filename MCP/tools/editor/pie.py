"""PIE lifecycle tools for the editor domain."""

import logging
from typing import Literal

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def register_editor_pie_tools(mcp, connection: UEConnection):
    """Register PIE tools."""

    @mcp.tool()
    def get_pie_state() -> str:
        """Get current PIE state.

        No caching: PIE state is dynamic and changes frequently.
        """
        try:
            response = connection.send_command("editor.get_pie_state")
            return format_response(response.get("data", {}), "get_pie_state")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def start_pie(
        mode: Literal["selected_viewport", "new_window"] = "selected_viewport",
        map: str = "",
        game_mode: str = "",
    ) -> str:
        """Start PIE and wait for completion."""
        params = {"mode": mode}
        if map:
            params["map"] = map
        if game_mode:
            params["game_mode"] = game_mode
        try:
            response = connection.send_command("editor.start_pie", params, timeout=60.0)
            return format_response(response.get("data", {}), "start_pie")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def stop_pie() -> str:
        """Stop PIE and wait for completion."""
        try:
            response = connection.send_command("editor.stop_pie", {}, timeout=30.0)
            return format_response(response.get("data", {}), "stop_pie")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def pause_pie() -> str:
        """Pause PIE session."""
        try:
            response = connection.send_command("editor.pause_pie")
            return format_response(response.get("data", {}), "pause_pie")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def resume_pie() -> str:
        """Resume paused PIE session."""
        try:
            response = connection.send_command("editor.resume_pie")
            return format_response(response.get("data", {}), "resume_pie")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def restart_pie(mode: Literal["selected_viewport", "new_window"] = "selected_viewport") -> str:
        """Restart PIE session (stop then start)."""
        try:
            response = connection.send_command("editor.restart_pie", {"mode": mode}, timeout=90.0)
            return format_response(response.get("data", {}), "restart_pie")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"
