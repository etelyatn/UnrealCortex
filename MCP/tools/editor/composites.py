"""Composite editor workflows built from primitive editor commands."""

import json
import logging

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection
from tools.editor import _input

logger = logging.getLogger(__name__)


def register_editor_composite_tools(mcp, connection: UEConnection):
    """Register composite editor tools."""

    @mcp.tool()
    def start_pie_session(
        mode: str = "selected_viewport",
        map: str = "",
        game_mode: str = "",
    ) -> str:
        """Start PIE then return current PIE state.

        Composite uses sequential calls, not batch, since PIE commands are deferred.
        """
        params = {"mode": mode}
        if map:
            params["map"] = map
        if game_mode:
            params["game_mode"] = game_mode
        try:
            connection.send_command("editor.start_pie", params, timeout=60.0)
            state = connection.send_command("editor.get_pie_state")
            return format_response(state.get("data", {}), "start_pie_session")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def stop_pie_session() -> str:
        """Stop PIE and return final state."""
        try:
            connection.send_command("editor.stop_pie", {}, timeout=30.0)
            state = connection.send_command("editor.get_pie_state")
            return format_response(state.get("data", {}), "stop_pie_session")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def press_key(key: str, action: str = "tap", duration_ms: int = 50) -> str:
        """Inject a key event into active PIE session."""
        try:
            response = _input.inject_key(connection, key, action=action, duration_ms=duration_ms)
            return format_response(response.get("data", {}), "press_key")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def run_input_sequence(steps_json: str, timeout: float = 60.0) -> str:
        """Execute deferred timed input sequence.

        Args:
            steps_json: JSON list of input steps with at_ms timestamps.
            timeout: Total timeout for deferred completion.
        """
        try:
            steps = json.loads(steps_json)
            response = _input.inject_sequence(connection, steps, timeout=timeout)
            return format_response(response.get("data", {}), "run_input_sequence")
        except json.JSONDecodeError as e:
            return f"Error: Invalid JSON in steps_json: {e}"
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"
