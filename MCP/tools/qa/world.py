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

    @mcp.tool()
    def probe_forward(distance: float = 3000.0, channel: str = "visibility") -> str:
        try:
            response = connection.send_command(
                "qa.probe_forward",
                {"distance": distance, "channel": channel},
            )
            return format_response(response.get("data", {}), "probe_forward")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def check_stuck(duration: float = 0.5, threshold: float = 10.0) -> str:
        try:
            response = connection.send_command(
                "qa.check_stuck",
                {"duration": duration, "threshold": threshold},
                timeout=duration + 5.0,
            )
            return format_response(response.get("data", {}), "check_stuck")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_visible_actors(
        max_distance: float = 5000.0,
        max_actors: int = 20,
        require_los: bool = True,
        tag: str = "",
    ) -> str:
        try:
            params = {
                "max_distance": max_distance,
                "max_actors": max_actors,
                "require_los": require_los,
            }
            if tag:
                params["tag"] = tag
            response = connection.send_command("qa.get_visible_actors", params)
            return format_response(response.get("data", {}), "get_visible_actors")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
