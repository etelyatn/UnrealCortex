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
    def interact_with(target: str = "", key: str = "E") -> str:
        try:
            params = {"key": key}
            if target:
                params["target"] = target
            response = connection.send_command("qa.interact", params)
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
