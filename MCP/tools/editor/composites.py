"""Composite editor workflows built from primitive editor commands."""

import logging

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

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
    def press_key(key: str, action: str = "tap", duration_ms: int = 100) -> str:
        """Inject a key event into active PIE session.

        Args:
            key: UE key name (for example "W", "SpaceBar", "LeftShift", "Enter",
                "Escape", "F1", "LeftMouseButton"). Case-sensitive.
            action: "tap" (press + timed release), "press", or "release".
            duration_ms: Hold duration in ms for "tap" action (default 100).
        """
        try:
            response = connection.send_command(
                "editor.inject_key",
                {"key": key, "action": action, "duration_ms": duration_ms},
            )
            return format_response(response.get("data", {}), "press_key")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def run_input_sequence(steps: list[dict], timeout: float = 60.0) -> str:
        """Execute deferred timed input sequence during PIE.

        Args:
            steps: List of input steps. Each step has:
                - at_ms (int): When to execute (ms from start)
                - kind (str): "key", "mouse", or "action"
                - For kind="key": key (str), action (str, default "tap"),
                  duration_ms (int, default 100)
                - For kind="mouse": action (str: "click"/"move"/"scroll"),
                  button (str, for click), x/y (float), delta (float, for scroll)
                - For kind="action": action_name (str), value (float, default 1.0)
            timeout: Total timeout for deferred completion (default 60s).

        Example:
            steps=[
                {"at_ms": 0, "kind": "key", "key": "W", "action": "press"},
                {"at_ms": 500, "kind": "key", "key": "SpaceBar", "action": "tap"},
                {"at_ms": 1000, "kind": "key", "key": "W", "action": "release"},
            ]
        """
        try:
            response = connection.send_command(
                "editor.inject_input_sequence",
                {"steps": steps},
                timeout=timeout,
            )
            return format_response(response.get("data", {}), "run_input_sequence")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"
