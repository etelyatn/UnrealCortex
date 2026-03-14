"""UMG domain tools for CortexMCP."""

from .composites import register_umg_composite_tools


def register_umg_tools(mcp, connection):
    """Register all UMG domain tools."""
    register_umg_composite_tools(mcp, connection)
