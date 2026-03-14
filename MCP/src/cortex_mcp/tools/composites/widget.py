"""Explicit registration for widget composite tools."""

from __future__ import annotations

import sys
from pathlib import Path

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.umg.composites import register_umg_composite_tools


def register_widget_compose_tools(mcp, connection) -> None:
    """Register widget composition tools."""
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_umg_composite_tools(_CaptureMCP(), connection)

    @mcp.tool(name="widget_compose")
    def widget_compose(**kwargs) -> str:
        return captured["create_widget_screen"](**kwargs)
