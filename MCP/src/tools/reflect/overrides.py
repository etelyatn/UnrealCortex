"""MCP tools for override queries."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_native_page

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

        Shows overridden functions/events and custom additions per child Blueprint — essential for
        understanding how Blueprints extend C++ base classes. `depth` is the inheritance levels to
        check and `limit` caps the returned children.

        Returns the per-child overridden_functions, overridden_events, custom_functions and
        custom_variables plus total_overrides.
        """
        try:
            params = {
                "class_name": class_name,
                "depth": depth,
                "limit": limit,
            }
            response = connection.send_command_cached(
                "reflect.find_overrides",
                params,
                ttl=_TTL_OVERRIDES,
            )
            return format_native_page(response.get("data", {}), "reflect.find_overrides", params)
        except ConnectionError as e:
            return f"Error: {e}"
