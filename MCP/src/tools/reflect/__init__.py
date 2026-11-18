"""Reflect domain tools for CortexMCP.

The thin wrappers over commands ``reflect_cmd`` already lists were removed (UnrealCortex
issue #225): ``get_dependencies``, ``get_referencers``, ``query_class_detail``,
``query_class_hierarchy``, ``query_overrides`` and ``query_usages`` duplicated the router
at ~5.7K characters of default tool listing. The remaining standalones wrap commands the
router does not list or add projection it cannot.
"""

from .cache import register_reflect_cache_tools
from .context import register_reflect_context_tools
from .impact import register_reflect_impact_tools


def register_reflect_tools(mcp, connection):
    """Register all Reflect domain tools."""
    register_reflect_cache_tools(mcp, connection)
    register_reflect_context_tools(mcp, connection)
    register_reflect_impact_tools(mcp, connection)
