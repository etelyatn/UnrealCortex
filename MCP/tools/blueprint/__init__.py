"""Blueprint domain tools for CortexMCP."""

from .assets import register_blueprint_asset_tools
from .class_defaults import register_blueprint_class_defaults_tools
from .structure import register_blueprint_structure_tools
from .composites import register_blueprint_composite_tools
from .analysis import register_blueprint_analysis_tools
from .level_blueprint import register_level_blueprint_tools
from .migration import register_blueprint_migration_tools


def register_blueprint_tools(mcp, connection):
    """Register all Blueprint domain tools."""
    register_blueprint_asset_tools(mcp, connection)
    register_blueprint_class_defaults_tools(mcp, connection)
    register_blueprint_structure_tools(mcp, connection)
    register_blueprint_composite_tools(mcp, connection)
    register_blueprint_analysis_tools(mcp, connection)
    register_level_blueprint_tools(mcp, connection)
    register_blueprint_migration_tools(mcp, connection)
