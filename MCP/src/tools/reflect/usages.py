"""MCP tools for cross-reference / usage queries."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_native_page

logger = logging.getLogger(__name__)

_TTL_USAGES = 120  # 2 min (short TTL per design doc — usages change frequently)


def register_reflect_usage_tools(mcp, connection: UEConnection):
    """Register usage query tools."""

    @mcp.tool()
    def query_usages(
        symbol: str,
        class_name: str,
        scope: str = "all",
        deep_scan: bool = False,
        path_filter: str = "",
        limit: int = 20,
        max_blueprints: int = 50,
    ) -> str:
        """Use when you need detailed graph-level symbol references across Blueprints.

        Goes deeper than `get_referencers` by returning exact node types
        (read/write/call/cast/spawn/component). Essential before renaming, removing, or changing a
        C++ member. `scope` is 'all' (default) or 'derived' (inheriting Blueprints only);
        `deep_scan=true` loads unloaded Blueprints for fuller coverage; `path_filter` scopes the scan.

        For a quick package-level dependency list use `get_referencers`; for full risk-scored change
        assessment use `impact_analysis`.

        Returns the per-Blueprint reference list with context and reference type, plus scan metadata
        (scope, blueprints_scanned, not_scanned).
        """
        try:
            params = {
                "symbol": symbol,
                "class_name": class_name,
                "scope": scope,
                "deep_scan": deep_scan,
                "path_filter": path_filter,
                "limit": limit,
                "max_blueprints": max_blueprints,
            }
            response = connection.send_command_cached(
                "reflect.find_usages",
                params,
                ttl=_TTL_USAGES,
            )
            return format_native_page(response.get("data", {}), "reflect.find_usages", params)
        except ConnectionError as e:
            return f"Error: {e}"
