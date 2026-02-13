"""Material domain tools for CortexMCP."""

from .assets import register_material_asset_tools
from .parameters import register_material_parameter_tools
from .graph import register_material_graph_tools
from .collections import register_material_collection_tools
from .composites import register_material_composite_tools


def register_material_tools(mcp, connection):
    """Register all Material domain tools."""
    register_material_asset_tools(mcp, connection)
    register_material_parameter_tools(mcp, connection)
    register_material_graph_tools(mcp, connection)
    register_material_collection_tools(mcp, connection)
    register_material_composite_tools(mcp, connection)
