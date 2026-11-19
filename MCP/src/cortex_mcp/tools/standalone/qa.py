"""Explicit registration for standalone QA tools."""

from __future__ import annotations

import json

from cortex_mcp.tool_output import canonical_failure
from tools.qa.composites import register_qa_composite_tools

_STEP_SHAPE = '{"command": "qa.<command>", "params": {...}}'


def _step_shape_error(field: str, step) -> str | None:
    """Refuse a malformed step before anything is sent to the editor."""
    if step is None and field != "action":
        return None
    if (
        isinstance(step, dict)
        and isinstance(step.get("command"), str)
        and step["command"]
        and isinstance(step.get("params", {}), dict)
    ):
        return None
    return json.dumps(canonical_failure(
        "INVALID_FIELD",
        f"{field} must be {_STEP_SHAPE} with a non-empty qualified command string and an "
        f"optional params object.",
    ))


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

    @mcp.tool(
        name="qa_test_step",
        description=(
            "Run one QA step in a single call: send the action, apply an optional wait, observe state, "
            "collect recent logs, run the optional assertion, and capture a screenshot. Returns "
            "action/wait/assert success, the finding count and the screenshot payload. `action`, "
            f"`wait` and `assertion` are each {_STEP_SHAPE}.\n"
        ),
    )
    def qa_test_step(
        action: dict,
        wait: dict | None = None,
        assertion: dict | None = None,
    ) -> str:
        for field, step in (("action", action), ("wait", wait), ("assertion", assertion)):
            error = _step_shape_error(field, step)
            if error is not None:
                return error
        return captured["test_step"](
            action=action,
            wait=wait,
            assertion=assertion,
        )
