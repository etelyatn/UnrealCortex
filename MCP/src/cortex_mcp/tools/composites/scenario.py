"""Explicit registration for QA scenario composition tools."""

from __future__ import annotations

from tools.qa.composites import register_qa_composite_tools


def register_scenario_compose_tools(mcp, connection) -> None:
    """Register the scenario composition tool under its explicit name."""
    # _CaptureMCP: intercept legacy registration, re-export under consolidated names.
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_qa_composite_tools(_CaptureMCP(), connection)
    run_scenario_inline = captured["run_scenario_inline"]

    @mcp.tool(
        name="scenario_compose",
        description=(
            "COMPOSITE tool — run a named QA scenario of ordered action steps in one call. Each step "
            "sends its command, then observes state and recent logs; findings are aggregated and a "
            "Markdown report bundle is written. Returns per-step success/findings and the report path. "
            "For individual QA commands use qa_cmd.\n"
        ),
    )
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
