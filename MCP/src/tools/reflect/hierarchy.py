"""MCP tools for class hierarchy queries."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_HIERARCHY = 1800  # 30 min


def _walk_tree(node: dict, parent_name: str | None, depth: int, out: list) -> None:
    """Recursively walk the tree and append flat entries."""
    entry = {
        "name": node.get("name", "Unknown"),
        "type": node.get("type", "cpp"),
        "parent": parent_name,
        "depth": depth,
        "children_count": len(node.get("children", [])),
    }
    for key in ("module", "source_path", "asset_path"):
        if key in node:
            entry[key] = node[key]
    out.append(entry)

    for child in node.get("children", []):
        _walk_tree(child, node.get("name", "Unknown"), depth + 1, out)


def _flatten_hierarchy(tree: dict) -> dict:
    """Convert C++ tree response to flat list for LLM consumption."""
    classes: list[dict] = []
    _walk_tree(tree, parent_name=None, depth=0, out=classes)
    result = {
        "classes": classes,
        "total_classes": tree.get("total_classes", len(classes)),
        "cpp_count": tree.get("cpp_count", 0),
        "blueprint_count": tree.get("blueprint_count", 0),
    }
    # The walk's own exhaustion signal. Projecting the tree away must not lose it: emitted child
    # counts cannot tell a caller whether the traversal stopped early.
    if "truncated" in tree:
        result["truncated"] = bool(tree["truncated"])
    return result


def register_reflect_hierarchy_tools(mcp, connection: UEConnection):
    """Register class hierarchy query tools."""

    @mcp.tool()
    def query_class_hierarchy(
        root: str,
        depth: int = 2,
        include_blueprint: bool = True,
        include_engine: bool = False,
        max_results: int = 100,
    ) -> str:
        """Use when you need to understand the inheritance tree of a C++ or Blueprint class.

        Shows parent/child relationships across C++ and Blueprint boundaries and returns a flat list
        with parent/depth fields for easy reasoning. `include_engine=false` (default) keeps project
        classes only; `include_blueprint=false` drops Blueprint-derived classes; `max_results` caps
        the list.

        Returns the flat class list (name, type, parent, depth, module/asset_path) plus
        total_classes, cpp_count, blueprint_count and, when the native walk reports it, truncated
        (true means the depth/max_results walk stopped early, so the list is not exhaustive).
        """
        try:
            response = connection.send_command_cached(
                "reflect.class_hierarchy",
                {
                    "root": root,
                    "depth": depth,
                    "include_blueprint": include_blueprint,
                    "include_engine": include_engine,
                    "max_results": max_results,
                },
                ttl=_TTL_HIERARCHY,
            )
            data = response.get("data", {})
            flat = _flatten_hierarchy(data)
            return format_response(flat, "query_class_hierarchy")
        except ConnectionError as e:
            return f"Error: {e}"
