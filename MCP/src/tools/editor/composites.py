"""Composite editor workflows built from primitive editor commands."""

import json
import logging
import pathlib
import subprocess
import threading
import time

import psutil

from cortex_mcp.project import resolve_project_dir
from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UECommandError, UEConnection
from cortex_mcp.tool_output import CONNECTION_ERROR_CODE, canonical_failure

logger = logging.getLogger(__name__)


def do_shutdown_editor(connection: UEConnection, force: bool = False) -> dict:
    """Shut down the Unreal Editor and return a result dict.

    ``force`` defaults to ``False``: an editor with unsaved packages refuses with
    ``UNSAVED_CHANGES`` (listing them) instead of discarding them.
    """
    try:
        response = connection.send_command("core.shutdown", {"force": force})
        return response.get("data", {})
    except UECommandError as exc:
        return canonical_failure(exc.code, exc.message, exc.details)
    except (ConnectionError, RuntimeError):
        # Expected when editor closes socket during shutdown.
        return {
            "message": "Shutdown initiated",
            "force": force,
            "note": "Connection closed as expected",
        }


def _editor_exe_for(engine_root: pathlib.Path) -> pathlib.Path:
    return engine_root / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"


def _engine_from_running_editor(pid: int) -> tuple[pathlib.Path, pathlib.Path] | None:
    """Return (editor exe, engine root) of the process running ``pid``, or None."""
    try:
        exe = pathlib.Path(psutil.Process(pid).exe())
    except (psutil.Error, OSError):
        return None
    if exe.stem.lower() != "unrealeditor" or len(exe.parents) < 4:
        return None
    # <root>/Engine/Binaries/Win64/UnrealEditor.exe
    return exe, exe.parents[3]


def _registry_engine_root(association: str) -> pathlib.Path | None:
    """Resolve an EngineAssociation (build id or installed version) through the Windows registry."""
    try:
        import winreg
    except ImportError:
        return None

    lookups = (
        (winreg.HKEY_CURRENT_USER, r"Software\Epic Games\Unreal Engine\Builds", association),
        (
            winreg.HKEY_LOCAL_MACHINE,
            rf"SOFTWARE\EpicGames\Unreal Engine\{association}",
            "InstalledDirectory",
        ),
    )
    for hive, key_path, value_name in lookups:
        try:
            with winreg.OpenKey(hive, key_path) as key:
                value, _ = winreg.QueryValueEx(key, value_name)
        except OSError:
            continue
        if isinstance(value, str) and value:
            return pathlib.Path(value)
    return None


def _engine_from_association(project_path: str) -> tuple[pathlib.Path, pathlib.Path] | None:
    """Resolve (editor exe, engine root) from the .uproject EngineAssociation, or None."""
    try:
        data = json.loads(pathlib.Path(project_path).read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None
    association = data.get("EngineAssociation") if isinstance(data, dict) else None
    if not isinstance(association, str) or not association:
        return None

    candidate = pathlib.Path(association)
    if not candidate.is_absolute():
        candidate = pathlib.Path(project_path).parent / candidate
    root = candidate if candidate.is_dir() else _registry_engine_root(association)
    if root is None:
        return None
    exe = _editor_exe_for(root)
    return (exe, root) if exe.is_file() else None


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


def _open_probe(host: str, port: int) -> UEConnection:
    """A connection pinned to one port (an explicit port is never rediscovered)."""
    return UEConnection(host=host, port=port)


def _read_port_file(port_file: pathlib.Path) -> dict | None:
    try:
        content = port_file.read_text().strip()
        if content.startswith("{"):
            data = json.loads(content)
            return data if isinstance(data, dict) else None
    except (json.JSONDecodeError, OSError):
        pass
    return None


def _locate_running_editor(
    connection: UEConnection, saved_dir: pathlib.Path
) -> tuple[int | None, str | None]:
    """Return (pid, project_path) of the connected editor.

    The pid comes from the connection, else from the port file of the connected port.
    """
    pid = connection._pid
    project_path = connection._project_path

    if not pid:
        port = getattr(connection, "port", None)
        for port_file in sorted(saved_dir.glob("CortexPort-*.txt")):
            data = _read_port_file(port_file)
            if not data or not port or data.get("port") != port:
                continue
            found_pid = data.get("pid")
            if not isinstance(found_pid, int):
                suffix = port_file.stem.removeprefix("CortexPort-")
                found_pid = int(suffix) if suffix.isdigit() else None
            if found_pid:
                pid = found_pid
                project_path = project_path or data.get("project_path")
                break

    if pid and not project_path:
        data = _read_port_file(saved_dir / f"CortexPort-{pid}.txt")
        if data:
            project_path = data.get("project_path")

    return pid, project_path


def do_restart_editor(
    connection: UEConnection,
    timeout: int = 120,
    cancel: threading.Event | None = None,
    force: bool = False,
) -> dict:
    """Restart the Unreal Editor: shutdown, relaunch, and verify MCP.

    The relaunched editor is the engine of the running one (its executable), falling back to
    the .uproject EngineAssociation. Success requires the old editor to have exited, the
    launched process to be alive, a port file written by that very process, and ``get_status``
    to report the project.

    Args:
        connection: Active UEConnection to the editor.
        timeout: Total timeout in seconds for the full restart sequence.
        cancel: Optional threading.Event. If set between phases, returns early
                with {"cancelled": True}.
        force: Passed to ``core.shutdown``. When False, an editor with unsaved packages
               refuses to shut down and the refusal (``UNSAVED_CHANGES``) is returned.

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

    old_pid, project_path = _locate_running_editor(connection, saved_dir)

    if not project_path:
        return canonical_failure(
            "INVALID_OPERATION",
            "Cannot determine .uproject path. "
            "Ensure editor was started with enhanced port file.",
        )

    if not old_pid:
        return canonical_failure(
            "INVALID_OPERATION",
            "Cannot determine the running editor's pid (no connection pid and no "
            "Saved/CortexPort-*.txt file for the connected port). Nothing was shut down or launched.",
            {"shutdown": "not_attempted", "relaunch_attempted": False},
        )

    engine = _engine_from_running_editor(old_pid) or _engine_from_association(project_path)
    if engine is None:
        return canonical_failure(
            "INVALID_OPERATION",
            "Cannot resolve the Unreal Engine to launch: the running editor's executable is "
            "unavailable and the .uproject EngineAssociation does not resolve to an installed "
            "engine. Nothing was shut down or launched.",
            {"project_path": project_path, "shutdown": "not_attempted", "relaunch_attempted": False},
        )
    editor_exe, engine_root = engine

    try:
        lock_file.parent.mkdir(parents=True, exist_ok=True)
        lock_file.write_text(str(int(time.time())))

        # Phase 1: shutdown if running
        if psutil.pid_exists(old_pid):
            try:
                connection.send_command("core.shutdown", {"force": force})
            except UECommandError as exc:
                return canonical_failure(exc.code, exc.message, exc.details)
            except (ConnectionError, RuntimeError):
                pass

            connection.disconnect()

            remaining = timeout - (time.monotonic() - start_time)
            shutdown_deadline = time.monotonic() + min(30.0, max(0.0, remaining))
            while time.monotonic() < shutdown_deadline:
                if not psutil.pid_exists(old_pid):
                    break
                time.sleep(2)
            else:
                if psutil.pid_exists(old_pid):
                    return canonical_failure(
                        "INVALID_OPERATION",
                        "Editor did not exit within 30s. Kill manually if needed.",
                        {"pid": old_pid, "shutdown": "timeout"},
                    )

        if cancel and cancel.is_set():
            return {"cancelled": True}

        try:
            (saved_dir / f"CortexPort-{old_pid}.txt").unlink(missing_ok=True)
        except OSError:
            pass
        # Phase 2: launch the editor and wait for its own port file. If the launched process exits before it reports
        # a port, relaunch once; a process that is still running is never launched twice. Other editors of the
        # project publish port files into the same Saved/; only the launched process's own file counts.
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
                            "engine_path": str(engine_root),
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
            logger.warning(
                "Relaunched editor pid %s exited with code %s before ready; retrying once", process.pid, process.poll()
            )
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
                message = (
                    f"Relaunched editor exited with code {state['exit_code']} after {launch_attempts} launch "
                    "attempt(s); no relaunched editor is running. Launch the editor manually."
                )
            return canonical_failure("INVALID_OPERATION", message, details)

        new_pid = process.pid
        if cancel and cancel.is_set():
            return {"cancelled": True}

        # Phase 4: verify through a probe pinned to the launched editor's port. The shared connection can rediscover
        # and retarget on a transport failure (most recently started editor wins), so it must not be the verifier;
        # it is retargeted only after the launched editor proved to be the expected project.
        expected_project = pathlib.Path(project_path).stem
        verify_details = {
            "port": new_port,
            "pid": new_pid,
            "relaunch_attempted": True,
            "launch_attempts": launch_attempts,
            **_process_state(process),
        }
        probe = _open_probe(connection.host, new_port)
        try:
            status = probe.send_command("get_status")
            status_data = status.get("data", {})
            domains = status_data.get("subsystems", {})
        except (ConnectionError, RuntimeError) as exc:
            return canonical_failure(
                CONNECTION_ERROR_CODE,
                f"Verification failed: {exc}",
                verify_details,
            )
        finally:
            probe.disconnect()

        reported_project = status_data.get("project_name")
        if reported_project != expected_project:
            return canonical_failure(
                "INVALID_OPERATION",
                f"Verification failed: editor on port {new_port} reports project "
                f"'{reported_project}', expected '{expected_project}'.",
                verify_details,
            )
        reported_pid = status_data.get("pid")
        if reported_pid is not None and reported_pid != new_pid:
            return canonical_failure(
                "INVALID_OPERATION",
                f"Verification failed: editor on port {new_port} reports pid {reported_pid}, "
                f"expected the launched pid {new_pid}.",
                {**verify_details, "reported_pid": reported_pid},
            )

        connection.retarget(new_port, new_pid, expected_project)
        connection._project_path = project_path

        elapsed = time.monotonic() - start_time
        return {
            "message": "Editor restarted successfully",
            "port": new_port,
            "old_pid": old_pid,
            "new_pid": new_pid,
            "engine_path": str(engine_root),
            "project": expected_project,
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
    def shutdown_editor(force: bool = False) -> str:
        """Gracefully shut down the Unreal Editor.

        With unsaved packages and ``force=False`` the editor refuses (``UNSAVED_CHANGES``).
        """
        try:
            response = connection.send_command("core.shutdown", {"force": force})
            return format_response(response.get("data", {}), "shutdown_editor")
        except UECommandError as exc:
            return json.dumps(canonical_failure(exc.code, exc.message, exc.details), indent=2)
        except (ConnectionError, RuntimeError):
            return json.dumps(
                {
                    "message": "Shutdown initiated",
                    "force": force,
                    "note": "Connection closed as expected",
                }
            )

    @mcp.tool()
    def restart_editor(timeout: int = 120, force: bool = False) -> str:
        """Restart the Unreal Editor: shutdown, relaunch, and verify MCP."""
        return json.dumps(do_restart_editor(connection, timeout, force=force), indent=2)
