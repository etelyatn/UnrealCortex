"""MCP tools for composite class context queries."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response
from .detail import _NATIVE_DETAIL, _VALID_DETAILS, _prune_detail

logger = logging.getLogger(__name__)

_TTL_CONTEXT = 1800  # 30 min

# The child page is requested at one bounded size; the native response's own traversal-exhaustion
# signal (`truncated`) is the authoritative completeness proof for that page.
_CHILDREN_MAX_RESULTS = 20


def register_reflect_context_tools(mcp, connection: UEConnection):
    """Register composite context query tools."""

    @mcp.tool()
    def query_class_context(
        class_name: str,
        detail: str = "properties",
    ) -> str:
        """Use when you need the full picture of a class: its parent, its own details, and its children.

        Saves 3 separate calls: returns the parent summary, self at the requested `detail` level
        ('summary', 'properties', or 'full'), and children summaries in a single response. Parent
        and children always use the 'summary' level. Any other `detail` value is refused without
        contacting the editor. `children_complete` reports whether the bounded child page is a
        complete inventory.
        """
        if detail not in _VALID_DETAILS:
            return (
                f"Error: invalid detail '{detail}'; expected one of "
                f"{', '.join(_VALID_DETAILS)}"
            )
        try:
            # Self at the requested view: only 'summary' takes the reduced native read.
            self_response = connection.send_command_cached(
                "reflect.class_detail",
                {
                    "class_name": class_name,
                    "include_inherited": False,
                    "detail": _NATIVE_DETAIL[detail],
                },
                ttl=_TTL_CONTEXT,
            )
            self_data = self_response.get("data", {})

            result = {
                "self": _prune_detail(self_data, detail),
            }

            # Get parent via the native summary read (with error handling): the parent is only
            # ever presented as a summary, so full extraction would be wasted work.
            parent_name = self_data.get("parent")
            if parent_name:
                try:
                    parent_response = connection.send_command_cached(
                        "reflect.class_detail",
                        {"class_name": parent_name, "include_inherited": False, "detail": "summary"},
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
                    {"root": class_name, "depth": 1, "max_results": _CHILDREN_MAX_RESULTS},
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
                # Completeness comes solely from the native traversal-exhaustion signal, never from
                # the emitted child count: filtered/skipped intermediaries and deeper nodes consume
                # the same native budget, so a bounded walk can emit fewer children than the cap.
                # `class_hierarchy` always emits `truncated`; the default only covers a
                # non-conforming payload.
                result["children_complete"] = not bool(hierarchy_data.get("truncated", False))
            except (ConnectionError, RuntimeError) as e:
                result["children"] = []
                result["children_count"] = 0
                result["children_error"] = str(e)

            return format_response(result, "query_class_context")
        except ConnectionError as e:
            return f"Error: {e}"
