"""QA action tools."""

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection


def register_qa_action_tools(mcp, connection: UEConnection):
    """Register QA action tools."""

    @mcp.tool()
    def move_player_to(
        target: str | list[float],
        timeout: float = 15.0,
        acceptance_radius: float = 75.0,
    ) -> str:
        try:
            params: dict = {
                "target": target,
                "timeout": timeout,
                "acceptance_radius": acceptance_radius,
            }
            response = connection.send_command("qa.move_to", params, timeout=timeout + 5.0)
            return format_response(response.get("data", {}), "move_player_to")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def interact_with(target: str = "", key: str = "E", duration: float = 0.1) -> str:
        """Simulate a key press interaction in the game world.

        Presses the specified key for the given duration then releases it.
        Use duration=0.1 (default) for a quick tap/click. Use longer durations
        (e.g., 2.0) for hold-to-interact mechanics like charging or soldering.

        Args:
            target: Optional actor name to look at before interacting.
            key: Unreal key name to press (default: "E"). Common values:
                "E" (interact), "LeftMouseButton", "RightMouseButton", "F".
            duration: How long to hold the key in seconds. 0.1 = tap, 2.0 = hold.
                Clamped to [0.01, 30.0] on the C++ side.
        """
        try:
            params = {"key": key, "duration": duration}
            if target:
                params["target"] = target
            response = connection.send_command("qa.interact", params, timeout=duration + 5.0)
            return format_response(response.get("data", {}), "interact_with")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def look_at_target(target: str | list[float]) -> str:
        try:
            response = connection.send_command("qa.look_at", {"target": target})
            return format_response(response.get("data", {}), "look_at_target")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def wait_for_condition(
        type: str,
        timeout: float = 5.0,
        actor: str = "",
        property: str = "",
        value: bool | float | str | int | None = None,
    ) -> str:
        try:
            params: dict = {"type": type, "timeout": timeout}
            if actor:
                params["actor"] = actor
            if property:
                params["property"] = property
            if value is not None:
                params["value"] = value
            response = connection.send_command("qa.wait_for", params, timeout=timeout + 5.0)
            return format_response(response.get("data", {}), "wait_for_condition")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
