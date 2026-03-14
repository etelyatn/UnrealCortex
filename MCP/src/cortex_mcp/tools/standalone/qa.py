"""Explicit registration for standalone QA tools."""

from __future__ import annotations

import sys
from pathlib import Path

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.qa.composites import register_qa_composite_tools


def register_qa_standalone_tools(mcp, connection) -> None:
    """Register QA standalone tools."""
    # _CaptureMCP: intercept legacy registration, re-export under consolidated names.
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_qa_composite_tools(_CaptureMCP(), connection)

    @mcp.tool(name="qa_test_step")
    def qa_test_step(
        action: dict,
        wait: dict | None = None,
        assertion: dict | None = None,
        screenshot_name: str = "qa_step.png",
    ) -> str:
        return captured["test_step"](
            action=action,
            wait=wait,
            assertion=assertion,
            screenshot_name=screenshot_name,
        )
