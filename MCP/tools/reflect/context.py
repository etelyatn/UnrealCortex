"""MCP tools for composite class context queries."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response
from .detail import _prune_detail

logger = logging.getLogger(__name__)

_TTL_CONTEXT = 1800  # 30 min


def register_reflect_context_tools(mcp, connection: UEConnection):
    """Register composite context query tools."""

    @mcp.tool()
    def query_class_context(
        class_name: str,
        detail: str = "properties",
    ) -> str:
        """Use when you need the full picture of a class: its parent, its own details, and its children.

        Saves 3 separate calls. Returns parent summary + self detail + children summaries
        in a single response.

        Args:
            class_name: Class name or Blueprint asset path.
            detail: Detail level for the main class — 'summary', 'properties', or 'full'.
                   Parent and children always use 'summary' level.

        Returns:
            Composite: parent (summary), self (at requested detail), children (summaries).
        """
        try:
            # Get full detail for the class (cached)
            self_response = connection.send_command_cached(
                "reflect.class_detail",
                {"class_name": class_name, "include_inherited": False},
                ttl=_TTL_CONTEXT,
            )
            self_data = self_response.get("data", {})

            result = {
                "self": _prune_detail(self_data, detail),
            }

            # Get parent summary (with error handling)
            parent_name = self_data.get("parent")
            if parent_name:
                try:
                    parent_response = connection.send_command_cached(
                        "reflect.class_detail",
                        {"class_name": parent_name, "include_inherited": False},
                        ttl=_TTL_CONTEXT,
                    )
                    result["parent"] = _prune_detail(
                        parent_response.get("data", {}), "summary"
                    )
                except (ConnectionError, RuntimeError) as e:
                    result["parent"] = {"name": parent_name, "error": str(e)}

            # Get children summaries via hierarchy (depth=1, cached)
            try:
                hierarchy_response = connection.send_command_cached(
                    "reflect.class_hierarchy",
                    {"root": class_name, "depth": 1, "max_results": 20},
                    ttl=_TTL_CONTEXT,
                )
                hierarchy_data = hierarchy_response.get("data", {})
                children = []
                for child in hierarchy_data.get("children", []):
                    entry = {"name": child.get("name"), "type": child.get("type")}
                    # Only include non-null optional fields (saves tokens)
                    if child.get("module"):
                        entry["module"] = child["module"]
                    if child.get("asset_path"):
                        entry["asset_path"] = child["asset_path"]
                    children.append(entry)
                result["children"] = children
                result["children_count"] = len(children)
            except (ConnectionError, RuntimeError):
                result["children"] = []
                result["children_count"] = 0

            return format_response(result, "query_class_context")
        except ConnectionError as e:
            return f"Error: {e}"
