"""Explicit registration for standalone editor tools."""

from __future__ import annotations

import sys
from pathlib import Path

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.editor.composites import register_editor_composite_tools


def register_editor_standalone_tools(mcp, connection) -> None:
    """Register standalone editor tools."""
    # _CaptureMCP: intercept legacy registration, re-export under consolidated names.
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_editor_composite_tools(_CaptureMCP(), connection)

    @mcp.tool(name="editor_restart")
    def editor_restart(timeout: int = 120) -> str:
        return captured["restart_editor"](timeout=timeout)
