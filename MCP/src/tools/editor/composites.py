"""Composite editor workflows built from primitive editor commands."""

import json
import logging
import os
import pathlib
import subprocess
import threading
import time

import psutil

from cortex_mcp.project import resolve_project_dir
from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.tool_output import CONNECTION_ERROR_CODE, canonical_failure

logger = logging.getLogger(__name__)


def do_shutdown_editor(connection: UEConnection, force: bool = True) -> dict:
    """Shut down the Unreal Editor and return a result dict."""
    try:
        response = connection.send_command("core.shutdown", {"force": force})
        return response.get("data", {})
    except (ConnectionError, RuntimeError):
        # Expected when editor closes socket during shutdown.
        return {
            "message": "Shutdown initiated",
            "force": force,
            "note": "Connection closed as expected",
        }


# One relaunch is retried when the first launched editor process exits before it reports a Cortex port.
_MAX_LAUNCH_ATTEMPTS = 2


def _launch_editor(editor_exe: pathlib.Path, project_path: str) -> subprocess.Popen:
    return subprocess.Popen(
        [
            str(editor_exe),
            project_path,
            "-nosplash",
            "-nopause",
            "-AutoDeclinePackageRecovery",
            '-ExecCmds="Mainframe.ShowRestoreAssetsPromptOnStartup 0"',
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def _read_launched_port(saved_dir: pathlib.Path, pid: int) -> int | None:
    """Port the editor process ``pid`` published in its own port file, else None."""
    try:
        data = json.loads((saved_dir / f"CortexPort-{pid}.txt").read_text().strip())
        return int(data["port"]) if data.get("pid") == pid else None
    except (json.JSONDecodeError, ValueError, OSError, KeyError, TypeError, AttributeError):
        return None


def _process_state(process: subprocess.Popen) -> dict:
    exit_code = process.poll()
    state = {"editor_pid": process.pid, "editor_running": exit_code is None}
    if exit_code is not None:
        state["exit_code"] = exit_code
    return state


def do_restart_editor(
    connection: UEConnection,
    timeout: int = 120,
    cancel: threading.Event | None = None,
) -> dict:
    """Restart the Unreal Editor: shutdown, relaunch, and verify MCP.

    Args:
        connection: Active UEConnection to the editor.
        timeout: Total timeout in seconds for the full restart sequence.
        cancel: Optional threading.Event. If set between phases, returns early
                with {"cancelled": True}.

    Returns:
        A dict with restart results or an error dict.
    """
    start_time = time.monotonic()

    resolved_dir = resolve_project_dir()
    if resolved_dir is None:
        return canonical_failure(
            "INVALID_OPERATION",
            "Cannot determine project directory. Set CORTEX_PROJECT_DIR or CLAUDE_PROJECT_DIR.",
        )

    saved_dir = resolved_dir / "Saved"
    lock_file = saved_dir / "CortexRestarting.lock"

    current_pid = connection._pid
    project_path = connection._project_path

    if not project_path:
        for _pf in saved_dir.glob("CortexPort-*.txt"):
            try:
                content = _pf.read_text().strip()
                if content.startswith("{"):
                    data = json.loads(content)
                    found_path = data.get("project_path")
                    if found_path:
                        project_path = found_path
                        if not current_pid:
                            current_pid = data.get("pid")
                        break
            except (json.JSONDecodeError, OSError):
                continue

    if not project_path:
        return canonical_failure(
            "INVALID_OPERATION",
            "Cannot determine .uproject path. "
            "Ensure editor was started with enhanced port file.",
        )

    # Resolve the launch before shutting anything down: a restart that cannot relaunch must leave the editor open.
    engine_path = os.environ.get("UE_56_PATH", "")
    editor_exe = pathlib.Path(engine_path) / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"
    if not engine_path or not editor_exe.is_file():
        reason = "UE_56_PATH not set" if not engine_path else f"{editor_exe} not found"
        return canonical_failure(
            "INVALID_OPERATION",
            f"{reason} - cannot launch editor; the running editor was left open",
            {"shutdown": "not_attempted", "relaunch_attempted": False},
        )

    try:
        lock_file.parent.mkdir(parents=True, exist_ok=True)
        lock_file.write_text(str(int(time.time())))

        # Phase 1: shutdown if running
        if current_pid and psutil.pid_exists(current_pid):
            try:
                connection.send_command("core.shutdown", {"force": True})
            except (ConnectionError, RuntimeError):
                pass

            connection.disconnect()

            remaining = timeout - (time.monotonic() - start_time)
            shutdown_deadline = time.monotonic() + min(30.0, max(0.0, remaining))
            while time.monotonic() < shutdown_deadline:
                if not psutil.pid_exists(current_pid):
                    break
                time.sleep(2)
            else:
                if psutil.pid_exists(current_pid):
                    return canonical_failure(
                        "INVALID_OPERATION",
                        "Editor did not exit within 30s. Kill manually if needed.",
                        {"pid": current_pid, "shutdown": "timeout"},
                    )

        if cancel and cancel.is_set():
            return {"cancelled": True}

        if current_pid:
            old_port_file = saved_dir / f"CortexPort-{current_pid}.txt"
            try:
                old_port_file.unlink(missing_ok=True)
            except OSError:
                pass

        # Phase 2: launch editor and wait for its new port file. If the launched process exits before it
        # reports a port, relaunch once; a process that is still running is never launched twice.
        remaining = timeout - (time.monotonic() - start_time)
        launch_deadline = time.monotonic() + max(0.0, remaining)
        process = None
        launch_attempts = 0
        new_port = None

        while True:
            if process is None:
                launch_attempts += 1
                try:
                    process = _launch_editor(editor_exe, project_path)
                except OSError as exc:
                    return canonical_failure(
                        "INVALID_OPERATION",
                        f"Editor relaunch failed: {exc}. No relaunched editor is running; launch the editor manually.",
                        {
                            "launch": "failed",
                            "relaunch_attempted": True,
                            "launch_attempts": launch_attempts,
                            "editor_running": False,
                        },
                    )
                if cancel and cancel.is_set():
                    return {"cancelled": True}

            if time.monotonic() >= launch_deadline:
                break
            time.sleep(3)
            if cancel and cancel.is_set():
                return {"cancelled": True}
            # Only the launched process's own port file counts, and only while that process is alive: a port file
            # left by a launch that died is stale.
            if process.poll() is None:
                new_port = _read_launched_port(saved_dir, process.pid)
                if new_port is not None:
                    break
                continue
            if launch_attempts >= _MAX_LAUNCH_ATTEMPTS:
                break
            # Relaunch even when the deadline has passed: a starting editor, reported as such, beats a closed one.
            logger.warning("Relaunched editor pid %s exited with code %s before ready; retrying once", process.pid, process.poll())
            process = None

        if new_port is None:
            state = _process_state(process)
            running = state["editor_running"]
            details = {
                "launch": "timeout" if running else "exited",
                "relaunch_attempted": True,
                "launch_attempts": launch_attempts,
                **state,
            }
            if running:
                message = (
                    f"Relaunched editor (pid {process.pid}) did not report a Cortex port within {timeout}s; "
                    "it is still running and may still be starting."
                )
            else:
                exit_code = state["exit_code"]
                message = (
                    f"Relaunched editor exited with code {exit_code} after {launch_attempts} launch attempt(s); "
                    "no relaunched editor is running. Launch the editor manually."
                )
            return canonical_failure("INVALID_OPERATION", message, details)

        new_pid = process.pid
        if cancel and cancel.is_set():
            return {"cancelled": True}

        # Phase 3: verify
        connection.port = new_port
        connection._pid = new_pid
        connection._project_path = project_path

        try:
            status = connection.send_command("get_status")
            domains = status.get("data", {}).get("subsystems", {})
        except (ConnectionError, RuntimeError) as exc:
            return canonical_failure(
                CONNECTION_ERROR_CODE,
                f"Verification failed: {exc}",
                {
                    "port": new_port,
                    "pid": new_pid,
                    "relaunch_attempted": True,
                    "launch_attempts": launch_attempts,
                    **_process_state(process),
                },
            )

        elapsed = time.monotonic() - start_time
        return {
            "message": "Editor restarted successfully",
            "port": new_port,
            "pid": new_pid,
            "project": pathlib.Path(project_path).stem,
            "domains": domains,
            "launch_attempts": launch_attempts,
            "restart_time_seconds": round(elapsed, 1),
        }
    finally:
        lock_file.unlink(missing_ok=True)


def register_editor_composite_tools(mcp, connection: UEConnection):
    """Register composite editor tools."""

    @mcp.tool()
    def start_pie_session(
        mode: str = "selected_viewport",
    ) -> str:
        """Start PIE then return current PIE state.

        Composite uses sequential calls, not batch, since PIE commands are deferred.
        """
        params = {"mode": mode}
        try:
            connection.send_command("editor.start_pie", params, timeout=60.0)
            state = connection.send_command("editor.get_pie_state")
            return format_response(state.get("data", {}), "start_pie_session")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def stop_pie_session() -> str:
        """Stop PIE and return final state."""
        try:
            connection.send_command("editor.stop_pie", {}, timeout=30.0)
            state = connection.send_command("editor.get_pie_state")
            return format_response(state.get("data", {}), "stop_pie_session")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def press_key(key: str, action: str = "tap", duration_ms: int = 100) -> str:
        """Inject a single key event into active PIE session.

        Use this for simple, single-key inputs (jump, confirm, cancel, etc.).
        For timed multi-step sequences, use run_input_sequence instead.

        Args:
            key: UE key name (for example "W", "SpaceBar", "LeftShift", "Enter",
                "Escape", "F1", "LeftMouseButton"). Case-sensitive.
            action: "tap" (press + timed release), "press", or "release".
            duration_ms: Hold duration in ms for "tap" action (default 100).

        Returns:
            Includes `dispatched` from Slate event handling. For `action="tap"`,
            `dispatched` reflects whether the press event was consumed by a widget.
            `false` means the event was dispatched but not handled.
        """
        try:
            response = connection.send_command(
                "editor.inject_key",
                {"key": key, "action": action, "duration_ms": duration_ms},
            )
            return format_response(response.get("data", {}), "press_key")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def run_input_sequence(steps: list[dict], timeout: float = 60.0) -> str:
        """Execute a timed multi-step input sequence during PIE.

        Use this when you need multiple inputs at specific timestamps, or inputs
        spaced across time (e.g., walk forward then turn). For a single keypress,
        use press_key instead.

        Args:
            steps: List of input steps. Each step has:
                - at_ms (int): When to execute (ms from start)
                - kind (str): "key", "mouse", or "action"
                - For kind="key": key (str), action (str, default "tap"),
                  duration_ms (int, default 100)
                - For kind="mouse": action (str: "click"/"move"/"scroll"),
                  button (str, for click), x/y (float), delta (float, for scroll)
                - For kind="action": action_name (str), value (float, default 1.0)
            timeout: Total timeout for deferred completion (default 60s).

        Returns:
            On success, response data includes:
            - `steps_executed`
            - `total_duration_ms` (scheduled max step time)
            - `actual_duration_ms` (wall-clock elapsed time)
            On cancellation, command may return `OperationCancelled`.

        Example:
            steps=[
                {"at_ms": 0, "kind": "key", "key": "W", "action": "press"},
                {"at_ms": 500, "kind": "key", "key": "SpaceBar", "action": "tap"},
                {"at_ms": 1000, "kind": "key", "key": "W", "action": "release"},
            ]
        """
        try:
            response = connection.send_command(
                "editor.inject_input_sequence",
                {"steps": steps},
                timeout=timeout,
            )
            return format_response(response.get("data", {}), "run_input_sequence")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def shutdown_editor(force: bool = True) -> str:
        """Gracefully shut down the Unreal Editor."""
        try:
            response = connection.send_command("core.shutdown", {"force": force})
            return format_response(response.get("data", {}), "shutdown_editor")
        except (ConnectionError, RuntimeError):
            return json.dumps(
                {
                    "message": "Shutdown initiated",
                    "force": force,
                    "note": "Connection closed as expected",
                }
            )

    @mcp.tool()
    def restart_editor(timeout: int = 120) -> str:
        """Restart the Unreal Editor: shutdown, relaunch, and verify MCP."""
        return json.dumps(do_restart_editor(connection, timeout), indent=2)
