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
        path_prefixes: list[str] | None = None,
    ) -> str:
        """Use when you need to see what Blueprint children override from a parent class.

        Shows overridden functions/events and custom additions per child Blueprint — essential for
        understanding how Blueprints extend C++ base classes. `depth` is the inheritance levels to
        check and `limit` caps the returned children. For an interface class (a Blueprint Interface
        or a native UInterface) there are no derived classes: the result lists its implementing
        Blueprints under `implementers`, searched in `path_prefixes` (mounted content folders,
        default /Game); the interface result is read live, never from the cache.

        Returns the per-child overridden_functions, overridden_events, custom_functions and
        custom_variables plus total_overrides.
        """
        try:
            params = {
                "class_name": class_name,
                "depth": depth,
                "limit": limit,
            }
            if path_prefixes:
                params["path_prefixes"] = path_prefixes
                response = connection.send_command("reflect.find_overrides", params)
            else:
                response = connection.send_command_cached(
                    "reflect.find_overrides",
                    params,
                    ttl=_TTL_OVERRIDES,
                )
            return format_native_page(response.get("data", {}), "reflect.find_overrides", params)
        except ConnectionError as e:
            return f"Error: {e}"
