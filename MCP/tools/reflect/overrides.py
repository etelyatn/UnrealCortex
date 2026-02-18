"""MCP tools for override queries."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_OVERRIDES = 1800  # 30 min


def register_reflect_override_tools(mcp, connection: UEConnection):
    """Register override query tools."""

    @mcp.tool()
    def query_overrides(
        class_name: str,
        depth: int = 2,
        limit: int = 20,
    ) -> str:
        """Use when you need to see what Blueprint children override from a parent class.

        Shows overridden functions/events and custom additions per child Blueprint.
        Essential for understanding how Blueprints extend C++ base classes.

        Args:
            class_name: Parent class name or Blueprint asset path.
            depth: How many inheritance levels to check (default 2).
            limit: Maximum number of children to return (default 20).

        Returns:
            Per-child list of overridden_functions, overridden_events,
            custom_functions, custom_variables, plus total_overrides.
        """
        try:
            response = connection.send_command_cached(
                "reflect.find_overrides",
                {
                    "class_name": class_name,
                    "depth": depth,
                    "limit": limit,
                },
                ttl=_TTL_OVERRIDES,
            )
            return format_response(response.get("data", {}), "query_overrides")
        except ConnectionError as e:
            return f"Error: {e}"
