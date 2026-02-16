"""Blueprint domain tools for CortexMCP."""

from .assets import register_blueprint_asset_tools
from .structure import register_blueprint_structure_tools
from .composites import register_blueprint_composite_tools


def register_blueprint_tools(mcp, connection):
    """Register all Blueprint domain tools."""
    register_blueprint_asset_tools(mcp, connection)
    register_blueprint_structure_tools(mcp, connection)
    register_blueprint_composite_tools(mcp, connection)
