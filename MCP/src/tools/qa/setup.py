"""QA setup tools."""

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection


def register_qa_setup_tools(mcp, connection: UEConnection):
    """Register QA setup tools."""

    @mcp.tool()
    def teleport_player(location: list[float], rotation: list[float] | None = None) -> str:
        try:
            params: dict = {"location": location}
            if rotation is not None:
                params["rotation"] = rotation
            response = connection.send_command("qa.teleport_player", params)
            return format_response(response.get("data", {}), "teleport_player")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_actor_property(actor: str, property: str, value: bool | float | str | int) -> str:
        try:
            params = {"actor": actor, "property": property, "value": value}
            response = connection.send_command("qa.set_actor_property", params)
            return format_response(response.get("data", {}), "set_actor_property")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_random_seed(seed: int) -> str:
        try:
            response = connection.send_command("qa.set_random_seed", {"seed": seed})
            return format_response(response.get("data", {}), "set_random_seed")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
