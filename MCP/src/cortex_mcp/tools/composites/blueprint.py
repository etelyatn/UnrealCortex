"""Explicit registration for blueprint composite tools."""

from __future__ import annotations

import sys
from pathlib import Path

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.blueprint.composites import register_blueprint_composite_tools


def register_blueprint_compose_tools(mcp, connection) -> None:
    """Register blueprint composition tools."""
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_blueprint_composite_tools(_CaptureMCP(), connection)

    @mcp.tool(name="blueprint_compose")
    def blueprint_compose(**kwargs) -> str:
        return captured["create_blueprint_graph"](**kwargs)
