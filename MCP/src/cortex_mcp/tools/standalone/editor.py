"""Explicit registration for standalone editor tools."""

from __future__ import annotations

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

    @mcp.tool(
        name="editor_restart",
        description=(
            "Restart the connected Unreal Editor: shut the editor down, relaunch it, and verify the "
            "MCP endpoint is reachable again. Returns the restart result. Disruptive to unsaved editor "
            "state; `timeout` bounds the wait for the editor to come back. Refuses before shutdown when "
            "the editor executable cannot be resolved; relaunches once if the new editor exits before it is "
            "ready. A failure reports relaunch_attempted, launch_attempts, editor_pid and editor_running.\n"
        ),
    )
    def editor_restart(timeout: int = 120) -> str:
        return captured["restart_editor"](timeout=timeout)
