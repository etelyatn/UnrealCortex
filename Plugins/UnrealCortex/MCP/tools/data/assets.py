"""MCP tools for asset search and discovery."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_SEARCH = 120  # 2 min


def register_asset_tools(mcp, connection: UEConnection):
    """Register all asset search-related MCP tools."""

    @mcp.tool()
    def search_assets(
        query: str = "",
        class_filter: str = "",
        path_filter: str = "",
        limit: int = 50,
    ) -> str:
        """Search assets in the Unreal Editor by name, class, or path.

        Searches the Asset Registry for matching assets. Use this to find assets
        when you don't know their exact path, or to explore what content exists
        in a particular directory or of a particular type.

        Args:
            query: Text to search for in asset names (case-insensitive substring match).
                   Leave empty to match all assets (use with class_filter or path_filter).
            class_filter: Optional class name to filter by (e.g., 'UDataTable', 'UTexture2D',
                          'UMaterialInstance'). Only returns assets of this class or its subclasses.
            path_filter: Optional prefix filter for asset paths (e.g., '/Game/Ripper/Products/').
                         Only returns assets whose path starts with this prefix.
            limit: Maximum number of results to return (default: 50, max: 500).

        Returns:
            JSON with:
            - assets: Array of matching assets, each containing:
              - name: Asset name
              - path: Full asset path
              - class_name: UClass name
            - total_count: Total number of matches (may exceed limit)
            - limit: Applied limit
        """
        try:
            params = {"limit": min(limit, 500)}
            if query:
                params["query"] = query
            if class_filter:
                params["class_filter"] = class_filter
            if path_filter:
                params["path_filter"] = path_filter
            response = connection.send_command_cached(
                "data.search_assets", params, ttl=_TTL_SEARCH
            )
            return format_response(response.get("data", {}), "search_assets")
        except ConnectionError as e:
            return f"Error: {e}"
