"""Core domain tools for CortexMCP."""

from tools.core.assets import register_core_asset_tools


def register_core_tools(mcp, connection):
    """Register all Core domain tools."""
    register_core_asset_tools(mcp, connection)
