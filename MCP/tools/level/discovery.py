"""MCP tools for discovery operations."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_level_discovery_tools(mcp, connection: UEConnection):
    """Register discovery MCP tools."""

    @mcp.tool()
    def list_actor_classes(category: str = "all") -> str:
        try:
            response = connection.send_command("level.list_actor_classes", {"category": category})
            return format_response(response.get("data", {}), "list_actor_classes")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def list_component_classes(category: str = "all") -> str:
        try:
            response = connection.send_command(
                "level.list_component_classes", {"category": category}
            )
            return format_response(response.get("data", {}), "list_component_classes")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def describe_class(class_name: str) -> str:
        try:
            response = connection.send_command("level.describe_class", {"class": class_name})
            return format_response(response.get("data", {}), "describe_class")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
