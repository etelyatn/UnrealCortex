"""MCP server exposing Unreal Engine systems to AI tools via UnrealCortex."""

import logging
import os
import sys
from mcp.server.fastmcp import FastMCP
from .capabilities import build_router_docstrings, load_capabilities_cache
from .response import format_response
from .schema_generator import _decode_data
from .tcp_client import UEConnection
from .tools.composites.blueprint import register_blueprint_compose_tools
from .tools.composites.level import register_level_compose_tools
from .tools.composites.material import register_material_compose_tools
from .tools.composites.scenario import register_scenario_compose_tools
from .tools.composites.widget import register_widget_compose_tools
from .tools.routers import register_router_tools
from .tools.standalone.editor import register_editor_standalone_tools
from .tools.standalone.qa import register_qa_standalone_tools
from .tools.standalone.schema import register_schema_standalone_tools
from tools.reflect import register_reflect_tools

_log_level = getattr(logging, os.environ.get("CORTEX_LOG_LEVEL", "INFO").upper(), logging.INFO)
logging.basicConfig(
    level=_log_level,
    stream=sys.stderr,
    format="%(levelname)s:%(name)s:%(message)s",
)
logger = logging.getLogger(__name__)

mcp = FastMCP("cortex")

# Build connection: CORTEX_PORT env var overrides port file discovery
_port_override = os.environ.get("CORTEX_PORT")
_connection = UEConnection(
    host=os.environ.get("CORTEX_HOST", "127.0.0.1"),
    port=int(_port_override) if _port_override else None,
)

_TTL_CATALOG = 600  # 10 min


def _register_explicit_tools(mcp_server, connection) -> None:
    """Register the consolidated 19-tool MCP surface."""
    docstrings = build_router_docstrings(load_capabilities_cache())
    register_router_tools(mcp_server, connection, docstrings)
    register_blueprint_compose_tools(mcp_server, connection)
    register_material_compose_tools(mcp_server, connection)
    register_widget_compose_tools(mcp_server, connection)
    register_level_compose_tools(mcp_server, connection)
    register_scenario_compose_tools(mcp_server, connection)
    register_editor_standalone_tools(mcp_server, connection)
    register_schema_standalone_tools(mcp_server, connection)
    register_qa_standalone_tools(mcp_server, connection)
    register_reflect_tools(mcp_server, connection)


def get_status() -> str:
    """Compatibility helper for tests; not MCP-registered."""
    from cortex_mcp.tcp_client import _discover_all_editors

    try:
        response = _connection.send_command("get_status")
        data = _decode_data(response)
        editors = _discover_all_editors()
        data["connected_editor"] = {"pid": _connection._pid, "port": _connection.port}
        data["available_editors"] = [
            {"pid": editor.pid, "port": editor.port, "started_at": editor.started_at}
            for editor in editors
        ]
        return format_response(data, "get_status")
    except ConnectionError as e:
        return f"Not connected to Unreal Editor: {e}"


def get_data_catalog() -> str:
    """Compatibility helper for tests; not MCP-registered."""
    try:
        response = _connection.send_command_cached(
            "data.get_data_catalog", {}, ttl=_TTL_CATALOG
        )
        return format_response(response.get("data", {}), "get_data_catalog")
    except ConnectionError as e:
        return f"Error: {e}"


def refresh_cache() -> str:
    """Compatibility helper for tests; not MCP-registered."""
    _connection.invalidate_cache(None)
    return '{"cleared": true}'
# Register explicit consolidated tools only.
_register_explicit_tools(mcp, _connection)


def main():
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
