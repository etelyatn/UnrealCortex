"""Explicit registration for material composite tools."""

from __future__ import annotations

import sys
from pathlib import Path

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.material.composites import register_material_composite_tools


def register_material_compose_tools(mcp, connection) -> None:
    """Register material composition tools."""
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_material_composite_tools(_CaptureMCP(), connection)

    @mcp.tool(name="material_compose")
    def material_compose(**kwargs) -> str:
        return captured["create_material_graph"](**kwargs)

    @mcp.tool(name="material_instance_compose")
    def material_instance_compose(**kwargs) -> str:
        return captured["create_material_instance"](**kwargs)
