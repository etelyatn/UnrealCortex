"""QA world-state query tools."""

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection


def register_qa_world_tools(mcp, connection: UEConnection):
    """Register QA world-state query tools."""

    @mcp.tool()
    def observe_game_state(
        radius: float = 5000.0,
        max_actors: int = 20,
        include_los: bool = False,
        interaction_range: float = 200.0,
    ) -> str:
        try:
            params = {
                "radius": radius,
                "max_actors": max_actors,
                "include_los": include_los,
                "interaction_range": interaction_range,
            }
            response = connection.send_command("qa.observe_state", params)
            return format_response(response.get("data", {}), "observe_game_state")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_actor_details(actor: str) -> str:
        try:
            response = connection.send_command("qa.get_actor_state", {"actor": actor})
            return format_response(response.get("data", {}), "get_actor_details")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_player_details() -> str:
        try:
            response = connection.send_command("qa.get_player_state", {})
            return format_response(response.get("data", {}), "get_player_details")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
