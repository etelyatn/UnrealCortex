"""MCP tools for component operations."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_level_component_tools(mcp, connection: UEConnection):
    """Register component MCP tools."""

    @mcp.tool()
    def list_components(actor: str) -> str:
        try:
            response = connection.send_command("level.list_components", {"actor": actor})
            return format_response(response.get("data", {}), "list_components")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def add_component(actor: str, class_name: str, name: str = "") -> str:
        try:
            params = {"actor": actor, "class": class_name}
            if name:
                params["name"] = name
            response = connection.send_command("level.add_component", params)
            return format_response(response.get("data", {}), "add_component")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def remove_component(actor: str, component: str) -> str:
        try:
            response = connection.send_command(
                "level.remove_component", {"actor": actor, "component": component}
            )
            return format_response(response.get("data", {}), "remove_component")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_component_property(actor: str, component: str, property_path: str) -> str:
        try:
            response = connection.send_command(
                "level.get_component_property",
                {"actor": actor, "component": component, "property": property_path},
            )
            return format_response(response.get("data", {}), "get_component_property")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_component_property(actor: str, component: str, property_path: str, value) -> str:
        try:
            response = connection.send_command(
                "level.set_component_property",
                {
                    "actor": actor,
                    "component": component,
                    "property": property_path,
                    "value": value,
                },
            )
            return format_response(response.get("data", {}), "set_component_property")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
