"""Explicit registration for level composite tools."""

from __future__ import annotations

import sys
from pathlib import Path

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.level.composites import register_level_composite_tools


def register_level_compose_tools(mcp, connection) -> None:
    """Register level composition tools."""
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_level_composite_tools(_CaptureMCP(), connection)

    @mcp.tool(name="level_compose")
    def level_compose(**kwargs) -> str:
        return captured["level_batch"](**kwargs)
