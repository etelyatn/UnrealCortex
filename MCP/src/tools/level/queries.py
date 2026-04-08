"""MCP tools for query and selection operations."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_BLUEPRINT_HINT = (
    "Use 'label' (e.g. BP_Lift) to target actors. "
    "'name' (BP_Lift_C_1) is the internal instance name. "
    "'class' (BP_Lift_C) is the compiled Blueprint class."
)


def _has_blueprint_actors(data: dict) -> bool:
    """Check if any actor in the response has a Blueprint class (ends with _C)."""
    for key in ("actors", "matches"):
        items = data.get(key, [])
        if isinstance(items, list):
            for item in items:
                if isinstance(item, dict):
                    cls = item.get("class", "")
                    if cls.endswith("_C"):
                        return True
    return False


def _inject_blueprint_hint(data: dict) -> None:
    """Add a naming convention hint if Blueprint actors are present."""
    if _has_blueprint_actors(data):
        data["hint"] = _BLUEPRINT_HINT


def register_level_query_tools(mcp, connection: UEConnection):
    """Register query MCP tools."""

    @mcp.tool()
    def list_actors(
        class_filter: str = "",
        tags: list[str] | None = None,
        folder: str = "",
        region: dict | None = None,
        limit: int = 100,
        offset: int = 0,
    ) -> str:
        try:
            params = {"limit": limit, "offset": offset}
            if class_filter:
                params["class"] = class_filter
            if tags is not None:
                params["tags"] = tags
            if folder:
                params["folder"] = folder
            if region is not None:
                params["region"] = region
            response = connection.send_command("level.list_actors", params)
            data = response.get("data", {})
            _inject_blueprint_hint(data)
            return format_response(data, "list_actors")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def find_actors(pattern: str, include_components: bool = False) -> str:
        try:
            params = {"pattern": pattern}
            if include_components:
                params["include_components"] = True
            response = connection.send_command("level.find_actors", params)
            data = response.get("data", {})
            _inject_blueprint_hint(data)
            return format_response(data, "find_actors")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_bounds(
        class_filter: str = "",
        tags: list[str] | None = None,
        folder: str = "",
        region: dict | None = None,
    ) -> str:
        try:
            params = {}
            if class_filter:
                params["class"] = class_filter
            if tags is not None:
                params["tags"] = tags
            if folder:
                params["folder"] = folder
            if region is not None:
                params["region"] = region
            response = connection.send_command("level.get_bounds", params)
            return format_response(response.get("data", {}), "get_bounds")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def select_actors(actors: list[str], add: bool = False) -> str:
        try:
            response = connection.send_command(
                "level.select_actors", {"actors": actors, "add": add}
            )
            return format_response(response.get("data", {}), "select_actors")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_selection() -> str:
        try:
            response = connection.send_command("level.get_selection", {})
            return format_response(response.get("data", {}), "get_selection")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
