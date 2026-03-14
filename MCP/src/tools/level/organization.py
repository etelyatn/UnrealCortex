"""MCP tools for organization operations."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_level_organization_tools(mcp, connection: UEConnection):
    """Register organization MCP tools."""

    @mcp.tool()
    def attach_actor(actor: str, parent: str, socket: str = "") -> str:
        try:
            params = {"actor": actor, "parent": parent}
            if socket:
                params["socket"] = socket
            response = connection.send_command("level.attach_actor", params)
            return format_response(response.get("data", {}), "attach_actor")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def detach_actor(actor: str) -> str:
        try:
            response = connection.send_command("level.detach_actor", {"actor": actor})
            return format_response(response.get("data", {}), "detach_actor")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_tags(actor: str, tags: list[str]) -> str:
        try:
            response = connection.send_command("level.set_tags", {"actor": actor, "tags": tags})
            return format_response(response.get("data", {}), "set_tags")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_folder(actor: str, folder: str = "") -> str:
        try:
            response = connection.send_command("level.set_folder", {"actor": actor, "folder": folder})
            return format_response(response.get("data", {}), "set_folder")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def group_actors(actors: list[str]) -> str:
        try:
            response = connection.send_command("level.group_actors", {"actors": actors})
            return format_response(response.get("data", {}), "group_actors")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def ungroup_actors(group: str) -> str:
        try:
            response = connection.send_command("level.ungroup_actors", {"group": group})
            return format_response(response.get("data", {}), "ungroup_actors")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
