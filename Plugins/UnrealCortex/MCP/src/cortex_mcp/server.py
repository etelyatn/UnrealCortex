"""MCP server exposing Unreal Engine systems to AI tools via UnrealCortex."""

import importlib.util
import json
import logging
import os
import pathlib
import sys
from mcp.server.fastmcp import FastMCP
from .tcp_client import UEConnection
from .response import format_response

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


def _discover_and_register_tools(mcp_server, connection):
    """Scan tools/ directory for register_*_tools functions."""
    tools_dir = pathlib.Path(__file__).parent.parent.parent / "tools"
    if not tools_dir.exists():
        logger.warning("Tools directory not found: %s", tools_dir)
        return

    for py_file in sorted(tools_dir.rglob("*.py")):
        if py_file.name.startswith("_"):
            continue
        module_name = py_file.stem
        spec = importlib.util.spec_from_file_location(f"cortex_tools.{module_name}", py_file)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        # Find and call register_*_tools functions
        for attr_name in dir(module):
            if attr_name.startswith("register_") and attr_name.endswith("_tools"):
                register_fn = getattr(module, attr_name)
                if callable(register_fn):
                    register_fn(mcp_server, connection)
                    logger.info("Registered tools from %s/%s", py_file.parent.name, py_file.name)


@mcp.tool()
def get_status() -> str:
    """Check connection status to Unreal Editor and get plugin/project info.

    Returns connection status, plugin version, engine version, and project name.
    Use this to verify the bridge is working before calling other tools.
    """
    try:
        response = _connection.send_command("get_status")
        return format_response(response.get("data", {}), "get_status")
    except ConnectionError as e:
        return f"Not connected to Unreal Editor: {e}"


@mcp.tool()
def get_data_catalog() -> str:
    """Get a compact overview of all project data in a single call.

    Returns DataTables (with top field names), GameplayTag prefixes, DataAsset classes,
    and StringTables. Useful when you need to understand the project's data structure
    or discover what tables/assets exist before making targeted queries.

    The catalog is cached for 10 minutes. Use refresh_cache if data has changed
    externally (e.g., new tables created in editor, C++ recompile).

    Returns:
        JSON with:
        - datatables: Array of {name, path, row_struct, row_count, is_composite,
          parent_tables, top_fields} for each loaded DataTable
        - tag_prefixes: Array of {prefix, count} for GameplayTag top-level categories
        - data_asset_classes: Array of {class_name, count, example_path} grouped by class
        - string_tables: Array of {name, path, entry_count} for each StringTable
    """
    try:
        response = _connection.send_command_cached(
            "get_data_catalog", {}, ttl=_TTL_CATALOG
        )
        return format_response(response.get("data", {}), "get_data_catalog")
    except ConnectionError as e:
        return f"Error: {e}"


@mcp.tool()
def refresh_cache() -> str:
    """Clear all cached data and force fresh reads from Unreal Editor.

    Use this when you know data has changed outside of MCP tools
    (e.g., new DataTables created in editor, C++ structs recompiled,
    assets imported). The cache also auto-clears on editor reconnect.

    Returns:
        JSON with cache stats before clearing.
    """
    stats = _connection._cache.stats
    _connection.invalidate_cache(None)
    return json.dumps({"cleared": True, "previous_stats": stats})


# Discover and register tools from tools/ directory
_discover_and_register_tools(mcp, _connection)


def main():
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
