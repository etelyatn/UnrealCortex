"""Level domain tools for CortexMCP."""

from .actors import register_level_actor_tools
from .transforms import register_level_transform_tools
from .components import register_level_component_tools
from .queries import register_level_query_tools
from .organization import register_level_organization_tools
from .discovery import register_level_discovery_tools
from .streaming import register_level_streaming_tools
from .composites import register_level_composite_tools


def register_level_tools(mcp, connection):
    """Register all Level domain tools."""
    register_level_actor_tools(mcp, connection)
    register_level_transform_tools(mcp, connection)
    register_level_component_tools(mcp, connection)
    register_level_query_tools(mcp, connection)
    register_level_organization_tools(mcp, connection)
    register_level_discovery_tools(mcp, connection)
    register_level_streaming_tools(mcp, connection)
    register_level_composite_tools(mcp, connection)
