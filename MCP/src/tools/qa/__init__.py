"""QA domain tools for CortexMCP."""

from .world import register_qa_world_tools
from .actions import register_qa_action_tools
from .setup import register_qa_setup_tools
from .assertions import register_qa_assertion_tools
from .composites import register_qa_composite_tools


def register_qa_tools(mcp, connection):
    """Register all QA domain tools."""
    register_qa_world_tools(mcp, connection)
    register_qa_action_tools(mcp, connection)
    register_qa_setup_tools(mcp, connection)
    register_qa_assertion_tools(mcp, connection)
    register_qa_composite_tools(mcp, connection)
