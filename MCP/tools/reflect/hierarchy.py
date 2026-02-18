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
    return {
        "classes": classes,
        "total_classes": tree.get("total_classes", len(classes)),
        "cpp_count": tree.get("cpp_count", 0),
        "blueprint_count": tree.get("blueprint_count", 0),
    }


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

        Shows parent/child relationships across C++ and Blueprint boundaries.
        Returns a flat list with parent/depth fields for easy reasoning.

        Args:
            root: Class name (e.g., 'ACharacter') or Blueprint asset path.
            depth: How many levels deep to traverse (default 2).
            include_blueprint: Include Blueprint-derived classes (default True).
            include_engine: Include engine classes (default False — project classes only).
            max_results: Maximum number of classes to return (default 100).

        Returns:
            Flat list of classes with name, type, parent, depth, module/asset_path,
            plus total_classes, cpp_count, blueprint_count.
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
