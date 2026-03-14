"""MCP tools for actor lifecycle operations."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_level_actor_tools(mcp, connection: UEConnection):
    """Register actor lifecycle MCP tools."""

    @mcp.tool()
    def spawn_actor(
        class_name: str,
        location: list[float] = [0.0, 0.0, 0.0],
        rotation: list[float] | None = None,
        scale: list[float] | None = None,
        label: str = "",
        folder: str = "",
        mesh: str = "",
        material: str = "",
    ) -> str:
        try:
            params = {"class": class_name, "location": location}
            if rotation is not None:
                params["rotation"] = rotation
            if scale is not None:
                params["scale"] = scale
            if label:
                params["label"] = label
            if folder:
                params["folder"] = folder
            if mesh:
                params["mesh"] = mesh
            if material:
                params["material"] = material
            response = connection.send_command("level.spawn_actor", params)
            return format_response(response.get("data", {}), "spawn_actor")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def delete_actor(actor: str, confirm_class: str = "") -> str:
        try:
            params = {"actor": actor}
            if confirm_class:
                params["confirm_class"] = confirm_class
            response = connection.send_command("level.delete_actor", params)
            return format_response(response.get("data", {}), "delete_actor")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def duplicate_actor(actor: str, offset: list[float] | None = None) -> str:
        try:
            params = {"actor": actor}
            if offset is not None:
                params["offset"] = offset
            response = connection.send_command("level.duplicate_actor", params)
            return format_response(response.get("data", {}), "duplicate_actor")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def rename_actor(actor: str, label: str) -> str:
        try:
            response = connection.send_command("level.rename_actor", {"actor": actor, "label": label})
            return format_response(response.get("data", {}), "rename_actor")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
