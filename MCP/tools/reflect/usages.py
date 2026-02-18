"""MCP tools for cross-reference / usage queries."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_USAGES = 120  # 2 min (short TTL per design doc — usages change frequently)


def register_reflect_usage_tools(mcp, connection: UEConnection):
    """Register usage query tools."""

    @mcp.tool()
    def query_usages(
        symbol: str,
        class_name: str,
        path_filter: str = "",
        limit: int = 20,
        max_blueprints: int = 50,
    ) -> str:
        """Use when you need to know where a property or function is referenced across Blueprints.

        Essential before renaming, removing, or changing the signature of a C++ member.
        Uses type-safe UK2Node casting for accurate, locale-independent detection.

        Args:
            symbol: Property or function name to search for (e.g., 'Health', 'TakeDamage').
            class_name: The class that defines the symbol.
            path_filter: Only search Blueprints under this path (e.g., '/Game/Blueprints/').
            limit: Maximum number of Blueprint classes to return (default 20).
            max_blueprints: Maximum number of Blueprint graphs to scan (default 50).

        Returns:
            Per-Blueprint reference list with context (graph name), type (read/write/call),
            plus total_usages and total_classes counts.
        """
        try:
            response = connection.send_command_cached(
                "reflect.find_usages",
                {
                    "symbol": symbol,
                    "class_name": class_name,
                    "path_filter": path_filter,
                    "limit": limit,
                    "max_blueprints": max_blueprints,
                },
                ttl=_TTL_USAGES,
            )
            return format_response(response.get("data", {}), "query_usages")
        except ConnectionError as e:
            return f"Error: {e}"
