"""Explicit registration for level composite tools."""

from __future__ import annotations

import sys
from pathlib import Path

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.level.composites import register_level_composite_tools

_LEVEL_DISAMBIG = (
    "COMPOSITE tool — use for batch actor operations. "
    "For individual level commands use level_cmd.\n\n"
)


def register_level_compose_tools(mcp, connection) -> None:
    """Register level composition tools."""
    # _CaptureMCP: intercept legacy registration, re-export under consolidated names.
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_level_composite_tools(_CaptureMCP(), connection)

    # Build description BEFORE decoration — FastMCP reads description= at decoration time
    _level_doc = _LEVEL_DISAMBIG + (captured.get("level_batch").__doc__ or "")

    @mcp.tool(name="level_compose", description=_level_doc)
    def level_compose(
        operations: list,
        stop_on_error: bool = False,
        save: bool = True,
    ) -> str:
        return captured["level_batch"](
            operations=operations,
            stop_on_error=stop_on_error,
            save=save,
        )
