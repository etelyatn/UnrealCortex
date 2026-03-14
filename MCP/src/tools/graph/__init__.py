"""Graph domain tools for CortexMCP."""

from .graph import register_graph_tools
from .layout import register_graph_layout_tools


def register_graph_domain_tools(mcp, connection):
    """Register all graph domain tools."""
    register_graph_tools(mcp, connection)
    register_graph_layout_tools(mcp, connection)
