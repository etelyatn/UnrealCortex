"""MCP tools for class detail queries."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_DETAIL = 1800  # 30 min

# Fields included at each detail level
# summary: enough to understand relationships
_SUMMARY_FIELDS = {
    "name", "type", "parent", "module", "asset_path",
    "source_path", "blueprint_children_count",
}
# properties: + structural data for working with variables
_PROPERTIES_FIELDS = _SUMMARY_FIELDS | {"properties", "interfaces", "components"}
# full: + function signatures (source snippets deferred from MVP)
_FULL_FIELDS = _PROPERTIES_FIELDS | {"functions"}


def _prune_detail(data: dict, detail: str) -> dict:
    """Prune response fields based on detail level."""
    if detail == "full":
        allowed = _FULL_FIELDS
    elif detail == "properties":
        allowed = _PROPERTIES_FIELDS
    else:
        allowed = _SUMMARY_FIELDS

    return {k: v for k, v in data.items() if k in allowed}


def register_reflect_detail_tools(mcp, connection: UEConnection):
    """Register class detail query tools."""

    @mcp.tool()
    def query_class_detail(
        class_name: str,
        include_inherited: bool = False,
        detail: str = "full",
    ) -> str:
        """Use when you need complete information about a single class.

        Returns properties, functions, interfaces, components, and source file path.
        Use detail='summary' for just name/type/parent, 'properties' for property list,
        'full' for everything including function signatures.

        Args:
            class_name: Class name (e.g., 'AMyCharacter') or Blueprint asset path.
            include_inherited: Include inherited properties/functions (default False).
            detail: Detail level — 'summary', 'properties', or 'full' (default 'full').

        Returns:
            Class metadata at the requested detail level.
        """
        try:
            response = connection.send_command_cached(
                "reflect.class_detail",
                {
                    "class_name": class_name,
                    "include_inherited": include_inherited,
                },
                ttl=_TTL_DETAIL,
            )
            data = response.get("data", {})
            pruned = _prune_detail(data, detail)
            return format_response(pruned, "query_class_detail")
        except ConnectionError as e:
            return f"Error: {e}"
