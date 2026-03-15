"""MCP tools for asset referencer queries."""

import logging
from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)

_TTL_REFS = 300  # 5 min


def register_reflect_referencer_tools(mcp, connection: UEConnection):
    """Register asset referencer query tools."""

    @mcp.tool()
    def get_referencers(
        asset_path: str,
        category: str = "all",
        limit: int = 100,
    ) -> str:
        """Use when you need a quick list of what references an asset.

        This is package-level and fast (Asset Registry, no loading). Use BEFORE
        destructive operations on any asset.

        For detailed graph-level usage (which nodes, read/write/call), use
        `query_usages`. For full risk-scored analysis before C++ changes, use
        `impact_analysis`.

        Args:
            asset_path: Asset or package path to check referencers for.
                Object paths with .AssetName suffix are OK.
            category: Filter referencers - 'all' or 'package' (default 'all').
            limit: Maximum referencers to return (default 100).

        Returns:
            List of referencers with package name, type (hard/soft), is_code flag,
            and module name or asset_class.
        """
        try:
            response = connection.send_command_cached(
                "reflect.get_referencers",
                {"asset_path": asset_path, "category": category, "limit": limit},
                ttl=_TTL_REFS,
            )
            return format_response(response.get("data", {}), "get_referencers")
        except ConnectionError as e:
            return f"Error: {e}"
