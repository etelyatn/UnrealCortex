"""PIE lifecycle tools for the editor domain."""

import logging
from typing import Literal

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def _inject_fps_throttle(connection: UEConnection) -> None:
    """Disable FPS throttling for AI-driven PIE sessions.

    UE throttles PIE to ~8 FPS when the editor loses focus. Since AI-driven
    control never focuses the window, this is always needed.
    """
    try:
        connection.send_command(
            "editor.execute_console_command", {"command": "t.MaxFPS 0"},
        )
        connection.send_command(
            "editor.execute_console_command",
            {"command": "t.UnfocusedFrameRateLimit 0"},
        )
    except (ConnectionError, RuntimeError):
        logger.warning("Failed to disable FPS throttle — PIE may run at reduced FPS")


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
        """Start PIE and wait for completion.

        Automatically disables FPS throttling (t.MaxFPS 0,
        t.UnfocusedFrameRateLimit 0) because the editor loses focus during
        AI-driven control, which would otherwise limit PIE to ~8 FPS.
        """
        params = {"mode": mode}
        if map:
            params["map"] = map
        if game_mode:
            params["game_mode"] = game_mode
        try:
            response = connection.send_command("editor.start_pie", params, timeout=60.0)
            _inject_fps_throttle(connection)
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
        """Restart PIE session (stop then start).

        Automatically disables FPS throttling after restart.
        """
        try:
            response = connection.send_command("editor.restart_pie", {"mode": mode}, timeout=90.0)
            _inject_fps_throttle(connection)
            return format_response(response.get("data", {}), "restart_pie")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"
