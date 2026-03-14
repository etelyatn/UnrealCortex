"""Explicit registration for widget composite tools."""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.umg.composites import register_umg_composite_tools

_WIDGET_DISAMBIG = (
    "COMPOSITE tool — use for creating a complete Widget Blueprint screen. "
    "For individual UMG commands use umg_cmd.\n\n"
)


def register_widget_compose_tools(mcp, connection) -> None:
    """Register widget composition tools."""
    # _CaptureMCP: intercept legacy registration, re-export under consolidated names.
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_umg_composite_tools(_CaptureMCP(), connection)

    # Build description BEFORE decoration — FastMCP reads description= at decoration time
    _widget_doc = _WIDGET_DISAMBIG + (captured.get("create_widget_screen").__doc__ or "")

    @mcp.tool(name="widget_compose", description=_widget_doc)
    def widget_compose(
        name: str,
        path: str,
        widgets: list[dict],
        animations: Optional[list[dict]] = None,
    ) -> str:
        return captured["create_widget_screen"](
            name=name,
            path=path,
            widgets=widgets,
            animations=animations,
        )
