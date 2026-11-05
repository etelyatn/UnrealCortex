"""MCP tools for asset referencer queries."""

import logging
from cortex_mcp.response import format_native_page
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
        include_config: bool | None = None,
    ) -> str:
        """Use when you need a quick list of what references an asset.

        Package-level and fast (Asset Registry, no loading); use BEFORE destructive operations on any
        asset. `category` filters to 'all' or 'package'. `include_config` (default true natively)
        controls whether INI/config references appear as type 'config' rows. For detailed graph-level
        usage (which nodes, read/write/call) use `query_usages`; for full risk-scored analysis before
        C++ changes use `impact_analysis`.

        Returns the referencer list with package name, hard/soft type, is_code flag, and module name
        or asset_class.
        """
        try:
            params = {"asset_path": asset_path, "category": category, "limit": limit}
            if include_config is not None:
                params["include_config"] = include_config
            response = connection.send_command_cached(
                "reflect.get_referencers",
                params,
                ttl=_TTL_REFS,
            )
            return format_native_page(response.get("data", {}), "reflect.get_referencers", params)
        except ConnectionError as e:
            return f"Error: {e}"
