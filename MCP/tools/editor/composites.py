"""Composite editor workflows built from primitive editor commands."""

import json
import logging
import os
import pathlib
import subprocess
import time

import psutil

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def register_editor_composite_tools(mcp, connection: UEConnection):
    """Register composite editor tools."""

    @mcp.tool()
    def start_pie_session(
        mode: str = "selected_viewport",
        map: str = "",
        game_mode: str = "",
    ) -> str:
        """Start PIE then return current PIE state.

        Composite uses sequential calls, not batch, since PIE commands are deferred.
        """
        params = {"mode": mode}
        if map:
            params["map"] = map
        if game_mode:
            params["game_mode"] = game_mode
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
        """Inject a key event into active PIE session.

        Args:
            key: UE key name (for example "W", "SpaceBar", "LeftShift", "Enter",
                "Escape", "F1", "LeftMouseButton"). Case-sensitive.
            action: "tap" (press + timed release), "press", or "release".
            duration_ms: Hold duration in ms for "tap" action (default 100).
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
        """Execute deferred timed input sequence during PIE.

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
            # Expected when editor closes socket during shutdown.
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
        start_time = time.monotonic()

        project_dir = os.environ.get("CORTEX_PROJECT_DIR", "")
        if not project_dir:
            return json.dumps({"error": "CORTEX_PROJECT_DIR not set"})

        saved_dir = pathlib.Path(project_dir) / "Saved"
        port_file = saved_dir / "CortexPort.txt"
        lock_file = saved_dir / "CortexRestarting.lock"

        current_pid = connection._pid
        project_path = connection._project_path

        if not project_path and port_file.exists():
            try:
                content = port_file.read_text().strip()
                if content.startswith("{"):
                    data = json.loads(content)
                    current_pid = current_pid or data.get("pid")
                    project_path = data.get("project_path")
            except (json.JSONDecodeError, OSError):
                pass

        if not project_path:
            return json.dumps(
                {
                    "error": "Cannot determine .uproject path. "
                    "Ensure editor was started with enhanced port file.",
                }
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

                shutdown_deadline = min(start_time + 30, start_time + timeout)
                while time.monotonic() < shutdown_deadline:
                    if not psutil.pid_exists(current_pid):
                        break
                    time.sleep(2)
                else:
                    if psutil.pid_exists(current_pid):
                        return json.dumps(
                            {
                                "error": "Shutdown timeout",
                                "pid": current_pid,
                                "message": "Editor did not exit within 30s. Kill manually if needed.",
                            }
                        )

            try:
                port_file.unlink(missing_ok=True)
            except OSError:
                pass

            # Phase 2: launch editor
            engine_path = os.environ.get("UE_56_PATH", "")
            if not engine_path:
                return json.dumps({"error": "UE_56_PATH not set - cannot launch editor"})

            editor_exe = (
                pathlib.Path(engine_path)
                / "Engine"
                / "Binaries"
                / "Win64"
                / "UnrealEditor.exe"
            )

            subprocess.Popen(
                [
                    str(editor_exe),
                    project_path,
                    "-nosplash",
                    "-nopause",
                    "-AutoDeclinePackageRecovery",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            # Phase 3: wait for new port
            remaining = timeout - (time.monotonic() - start_time)
            launch_deadline = time.monotonic() + max(0.0, remaining)
            new_port = None
            new_pid = None

            while time.monotonic() < launch_deadline:
                time.sleep(3)
                if port_file.exists():
                    try:
                        content = port_file.read_text().strip()
                        if content.startswith("{"):
                            data = json.loads(content)
                            new_pid = data.get("pid")
                            if new_pid and new_pid != current_pid:
                                new_port = int(data["port"])
                                break
                        else:
                            new_port = int(content)
                            break
                    except (json.JSONDecodeError, ValueError, OSError, KeyError):
                        continue

            if new_port is None:
                return json.dumps(
                    {
                        "error": "Launch timeout",
                        "message": f"Editor did not start within {timeout}s",
                    }
                )

            # Phase 4: verify
            connection.port = new_port
            connection._pid = new_pid
            connection._project_path = project_path

            try:
                status = connection.send_command("get_status")
                domains = status.get("data", {}).get("subsystems", {})
            except (ConnectionError, RuntimeError) as exc:
                return json.dumps(
                    {
                        "error": f"Verification failed: {exc}",
                        "port": new_port,
                        "pid": new_pid,
                    }
                )

            elapsed = time.monotonic() - start_time
            return json.dumps(
                {
                    "message": "Editor restarted successfully",
                    "port": new_port,
                    "pid": new_pid,
                    "project": pathlib.Path(project_path).stem,
                    "domains": domains,
                    "restart_time_seconds": round(elapsed, 1),
                },
                indent=2,
            )
        finally:
            lock_file.unlink(missing_ok=True)
