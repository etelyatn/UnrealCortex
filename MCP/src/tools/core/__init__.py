"""Core domain tools for CortexMCP."""

from .assets import register_core_asset_tools
from .asset_deletion import register_core_asset_deletion_tools
from .schema import register_schema_tools


def register_core_tools(mcp, connection):
    """Register all Core domain tools."""
    register_core_asset_tools(mcp, connection)
    register_core_asset_deletion_tools(mcp, connection)
    register_schema_tools(mcp, connection)
