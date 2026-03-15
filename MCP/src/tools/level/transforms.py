"""MCP tools for actor transform and property operations."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_level_transform_tools(mcp, connection: UEConnection):
    """Register transform/property MCP tools."""

    @mcp.tool()
    def get_actor(actor: str) -> str:
        try:
            response = connection.send_command("level.get_actor", {"actor": actor})
            return format_response(response.get("data", {}), "get_actor")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_transform(
        actor: str,
        location: list[float] | None = None,
        rotation: list[float] | None = None,
        scale: list[float] | None = None,
    ) -> str:
        try:
            params = {"actor": actor}
            if location is not None:
                params["location"] = location
            if rotation is not None:
                params["rotation"] = rotation
            if scale is not None:
                params["scale"] = scale
            response = connection.send_command("level.set_transform", params)
            return format_response(response.get("data", {}), "set_transform")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_actor_property(actor: str, property_path: str, value) -> str:
        try:
            response = connection.send_command(
                "level.set_actor_property",
                {"actor": actor, "property": property_path, "value": value},
            )
            return format_response(response.get("data", {}), "set_actor_property")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_actor_property(actor: str, property_path: str) -> str:
        try:
            response = connection.send_command(
                "level.get_actor_property",
                {"actor": actor, "property": property_path},
            )
            return format_response(response.get("data", {}), "get_actor_property")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
