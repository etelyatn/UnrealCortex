"""Editor domain tools for CortexMCP."""

from tools.editor.pie import register_editor_pie_tools
from tools.editor.viewport import register_editor_viewport_tools
from tools.editor.utilities import register_editor_utility_tools
from tools.editor.composites import register_editor_composite_tools


def register_editor_tools(mcp, connection):
    """Register all Editor domain tools."""
    register_editor_pie_tools(mcp, connection)
    register_editor_viewport_tools(mcp, connection)
    register_editor_utility_tools(mcp, connection)
    register_editor_composite_tools(mcp, connection)
