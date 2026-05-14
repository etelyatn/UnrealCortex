"""StateTree domain tools for CortexMCP."""

from .composites import register_statetree_composite_tools


def register_statetree_tools(mcp, connection):
    """Register StateTree tools exposed from this package."""
    register_statetree_composite_tools(mcp, connection)
