"""MCP tools for asset dependency queries."""

import logging
from cortex_mcp.response import format_native_page
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

        Typical examples include parent classes, referenced meshes/materials, and other Blueprints.
        Fast (Asset Registry, no loading); use BEFORE deleting or moving an asset to understand what
        it pulls in. `category` filters to 'all' or 'package'. For the reverse direction (what
        depends on THIS asset) use `get_referencers`; for risk-scored impact of C++ changes use
        `impact_analysis`.

        Returns the dependency list with package name, hard/soft type, is_code flag, and module name
        (C++) or asset_class (content).
        """
        try:
            params = {"asset_path": asset_path, "category": category, "limit": limit}
            response = connection.send_command_cached(
                "reflect.get_dependencies",
                params,
                ttl=_TTL_DEPS,
            )
            return format_native_page(response.get("data", {}), "reflect.get_dependencies", params)
        except ConnectionError as e:
            return f"Error: {e}"
