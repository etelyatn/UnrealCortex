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
            "Restart the connected Unreal Editor: shut the editor down, relaunch it with the same engine "
            "(the running editor's executable, else the .uproject EngineAssociation), and verify the "
            "MCP endpoint is reachable again. Returns `old_pid`, `new_pid`, `engine_path`, `launch_attempts` "
            "and the restart result. Refuses before shutdown, leaving the editor open, when the running "
            "editor's pid or its engine cannot be resolved; relaunches once if the new editor exits before it "
            "is ready. Success requires the old editor to exit, the launched process to publish its own port "
            "file and a probe pinned to that port to report the project. Disruptive to unsaved editor state; "
            "`timeout` bounds the wait for the editor to come back. `force` (default false) is passed to "
            "`core.shutdown`: with unsaved packages and force=false the restart is refused with "
            "UNSAVED_CHANGES listing `dirty_packages`; force=true discards them. A failure reports "
            "relaunch_attempted, launch_attempts, editor_pid and editor_running.\n"
        ),
    )
    def editor_restart(timeout: int = 120, force: bool = False) -> str:
        return captured["restart_editor"](timeout=timeout, force=force)
