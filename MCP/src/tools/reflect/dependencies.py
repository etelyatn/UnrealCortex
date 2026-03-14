"""MCP tools for asset dependency queries."""

import logging
from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)

_TTL_DEPS = 300  # 5 min


def register_reflect_dependency_tools(mcp, connection: UEConnection):
    """Register asset dependency query tools."""

    @mcp.tool()
    def get_dependencies(
        asset_path: str,
        category: str = "all",
        limit: int = 100,
    ) -> str:
        """Use when you need to know what an asset imports or loads.

        Typical examples include parent classes, referenced meshes/materials,
        and other Blueprints. Fast (Asset Registry, no loading).
        Use BEFORE deleting or moving an asset to understand what it pulls in.

        For the reverse direction (what depends on THIS asset), use
        `get_referencers`. For risk-scored impact of C++ changes, use
        `impact_analysis`.

        Args:
            asset_path: Asset or package path (e.g., '/Game/Blueprints/BP_Enemy'
                or '/Script/Engine'). Object paths with .AssetName suffix are OK.
            category: Filter dependencies - 'all' or 'package' (default 'all').
            limit: Maximum dependencies to return (default 100).

        Returns:
            List of dependencies with package name, type (hard/soft), is_code flag,
            and module name (for C++ deps) or asset_class (for content deps).
        """
        try:
            response = connection.send_command_cached(
                "reflect.get_dependencies",
                {"asset_path": asset_path, "category": category, "limit": limit},
                ttl=_TTL_DEPS,
            )
            return format_response(response.get("data", {}), "get_dependencies")
        except ConnectionError as e:
            return f"Error: {e}"
