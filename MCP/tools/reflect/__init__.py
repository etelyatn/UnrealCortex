"""Reflect domain tools for CortexMCP."""

from .hierarchy import register_reflect_hierarchy_tools
from .detail import register_reflect_detail_tools
from .overrides import register_reflect_override_tools
from .usages import register_reflect_usage_tools
from .cache import register_reflect_cache_tools
from .context import register_reflect_context_tools


def register_reflect_tools(mcp, connection):
    """Register all Reflect domain tools."""
    register_reflect_hierarchy_tools(mcp, connection)
    register_reflect_detail_tools(mcp, connection)
    register_reflect_override_tools(mcp, connection)
    register_reflect_usage_tools(mcp, connection)
    register_reflect_cache_tools(mcp, connection)
    register_reflect_context_tools(mcp, connection)
