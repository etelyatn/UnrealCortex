"""Explicit registration for QA scenario composition tools."""

from __future__ import annotations

import sys
from pathlib import Path

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.qa.composites import register_qa_composite_tools


def register_scenario_compose_tools(mcp, connection) -> None:
    """Register the scenario composition tool under its explicit name."""
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_qa_composite_tools(_CaptureMCP(), connection)
    run_scenario_inline = captured["run_scenario_inline"]

    @mcp.tool(name="scenario_compose")
    def scenario_compose(
        scenario_name: str,
        steps: list[dict],
        verbose: bool = False,
    ) -> str:
        return run_scenario_inline(
            scenario_name=scenario_name,
            steps=steps,
            verbose=verbose,
        )
