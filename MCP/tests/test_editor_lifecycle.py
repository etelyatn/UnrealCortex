"""Unit tests for editor lifecycle composite tools (shutdown, restart)."""

import json
import os
import pathlib
import threading
import types
from unittest.mock import MagicMock, patch

import psutil

from cortex_mcp.tcp_client import UECommandError, UEConnection
from tools.editor.composites import (
    do_restart_editor,
    do_shutdown_editor,
    register_editor_composite_tools,
)

OLD_PID = 1234
NEW_PID = 5678
PROJECT_PATH = "C:/test/Test.uproject"
RUNNING_EXE = str(pathlib.Path("C:/Engines/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe"))
RUNNING_ENGINE_ROOT = str(pathlib.Path("C:/Engines/UE_5.8"))


class MockMCP:
    """Captures tools registered via @mcp.tool()."""

    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator


def _port_file_text(pid, port, project_path=PROJECT_PATH):
    return json.dumps(
        {
            "port": port,
            "pid": pid,
            "project": "Test",
            "project_path": project_path,
            "started_at": "2026-01-01T00:00:00Z",
        }
    )


class RestartHarness:
    """Simulated editor world for do_restart_editor: pids, port files, shutdown and launch."""

    def __init__(self, tmp_path, *, old_pid=OLD_PID, status_project="Test", conn_pid=OLD_PID):
        self.saved_dir = tmp_path / "Saved"
        self.saved_dir.mkdir(parents=True)
        self.tmp_path = tmp_path
        self.alive = {old_pid} if old_pid else set()
        self.launched = []
        self.launch_pid = NEW_PID
        # One entry per launch: {"pid", "exit_code" or "poll_seq", "writes_file"}; the last entry repeats.
        self.launch_plan = None
        self.probe_error = None
        self.foreign_pids = []
        self.shutdown_error = None
        self.status_project = status_project
        self.process_exe = RUNNING_EXE
        self.old_pid = old_pid

        self.conn = MagicMock(spec=UEConnection)
        self.conn._pid = conn_pid
        self.conn._project_path = PROJECT_PATH
        self.conn.port = 8742
        self.conn.host = "127.0.0.1"
        self.conn.send_command.side_effect = self._send_command
        self.conn.retarget.side_effect = self._retarget
        # What the shared connection would answer if a transport failure made it rediscover another editor.
        self.rediscovered_status = None
        self.status_pid = None
        self.delay_launched_file = False
        self.sleeps = 0
        self.probe_ports = []
        self.probes = []
        if old_pid:
            (self.saved_dir / f"CortexPort-{old_pid}.txt").write_text(_port_file_text(old_pid, 8742))

    def _send_command(self, command, params=None, **_kwargs):
        if command == "core.shutdown":
            if self.shutdown_error is not None:
                raise self.shutdown_error
            self.alive.discard(self.old_pid)
            return {"success": True, "data": {"message": "Shutdown initiated"}}
        if command == "get_status":
            # The shared connection is never the verifier; it would answer for whichever editor discovery picks.
            return {"success": True, "data": self.rediscovered_status or self._status_data()}
        raise AssertionError(f"unexpected command {command}")

    def _status_data(self):
        data = {"project_name": self.status_project, "subsystems": {"core": True}}
        if self.status_pid is not None:
            data["pid"] = self.status_pid
        return data

    def _retarget(self, port, pid, project):
        self.conn.port = port
        self.conn._pid = pid

    def _open_probe(self, host, port):
        self.probe_ports.append(port)
        probe = MagicMock(spec=UEConnection)
        if self.probe_error is not None:
            probe.send_command.side_effect = self.probe_error
        else:
            probe.send_command.return_value = {"success": True, "data": self._status_data()}
        self.probes.append(probe)
        return probe

    def _sleep(self, _seconds):
        self.sleeps += 1
        if self.delay_launched_file and self.sleeps == 2:
            self._write_launched_file()

    def _write_launched_file(self, pid=None):
        pid = pid or self.launch_pid
        (self.saved_dir / f"CortexPort-{pid}.txt").write_text(_port_file_text(pid, 8743))

    def _popen(self, args, **_kwargs):
        self.launched.append(args)
        plans = self.launch_plan or [{"pid": self.launch_pid}]
        plan = plans[min(len(self.launched) - 1, len(plans) - 1)]
        pid = plan["pid"]
        poll_seq = list(plan.get("poll_seq", [plan.get("exit_code")]))
        if poll_seq[0] is None:
            self.alive.add(pid)
        for foreign_pid in self.foreign_pids:
            (self.saved_dir / f"CortexPort-{foreign_pid}.txt").write_text(_port_file_text(foreign_pid, 9999))
        if plan.get("writes_file", True) and not self.delay_launched_file:
            self._write_launched_file(pid)

        def poll():
            return poll_seq.pop(0) if len(poll_seq) > 1 else poll_seq[0]

        return types.SimpleNamespace(pid=pid, poll=poll)

    def _process(self, pid):
        if self.process_exe is None:
            raise psutil.NoSuchProcess(pid)
        return types.SimpleNamespace(exe=lambda: self.process_exe)

    def run(self, **restart_kwargs):
        env = {"CORTEX_PROJECT_DIR": str(self.tmp_path)}
        with patch.dict(os.environ, env, clear=False), \
                patch("tools.editor.composites.psutil.pid_exists", side_effect=lambda pid: pid in self.alive), \
                patch("tools.editor.composites.psutil.Process", side_effect=self._process), \
                patch("tools.editor.composites.subprocess.Popen", side_effect=self._popen), \
                patch("tools.editor.composites._open_probe", side_effect=self._open_probe), \
                patch("tools.editor.composites.time.sleep", side_effect=self._sleep):
            return do_restart_editor(self.conn, **restart_kwargs)


class TestShutdownEditor:
    """Tests for the shutdown_editor tool."""

    def test_shutdown_sends_core_shutdown(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.return_value = {
            "success": True,
            "data": {"message": "Shutdown initiated", "force": True},
        }

        mcp = MockMCP()
        register_editor_composite_tools(mcp, conn)

        assert "shutdown_editor" in mcp.tools
        mcp.tools["shutdown_editor"](force=True)
        conn.send_command.assert_called_with("core.shutdown", {"force": True})

    def test_shutdown_defaults_to_not_forcing(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.return_value = {"success": True, "data": {}}

        mcp = MockMCP()
        register_editor_composite_tools(mcp, conn)
        mcp.tools["shutdown_editor"]()
        conn.send_command.assert_called_with("core.shutdown", {"force": False})

    def test_shutdown_catches_connection_drop(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.side_effect = ConnectionError("Connection lost")

        mcp = MockMCP()
        register_editor_composite_tools(mcp, conn)

        result = mcp.tools["shutdown_editor"](force=True)
        payload = json.loads(result)
        assert payload["message"] == "Shutdown initiated"
        assert payload["force"] is True

    def test_shutdown_passes_unsaved_changes_refusal_through(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.side_effect = UECommandError(
            "core.shutdown",
            "UNSAVED_CHANGES",
            "2 unsaved packages",
            {"dirty_packages": [{"package": "/Game/A", "file": ""}], "count": 2},
        )

        mcp = MockMCP()
        register_editor_composite_tools(mcp, conn)
        payload = json.loads(mcp.tools["shutdown_editor"]())
        assert payload["success"] is False
        assert payload["_error"] == "UNSAVED_CHANGES"
        assert payload["count"] == 2


class TestRestartEditor:
    """Tests for restart_editor composite flow."""

    def test_restart_full_flow_uses_running_editor_engine(self, tmp_path):
        harness = RestartHarness(tmp_path)

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False):
            mcp = MockMCP()
            register_editor_composite_tools(mcp, harness.conn)
            assert "restart_editor" in mcp.tools
            with patch("tools.editor.composites.psutil.pid_exists", side_effect=lambda pid: pid in harness.alive), \
                    patch("tools.editor.composites.psutil.Process", side_effect=harness._process), \
                    patch("tools.editor.composites.subprocess.Popen", side_effect=harness._popen), \
                    patch("tools.editor.composites._open_probe", side_effect=harness._open_probe), \
                    patch("tools.editor.composites.time.sleep", side_effect=harness._sleep):
                payload = json.loads(mcp.tools["restart_editor"](timeout=30))

        assert payload["message"] == "Editor restarted successfully"
        assert payload["port"] == 8743
        assert payload["old_pid"] == OLD_PID
        assert payload["new_pid"] == NEW_PID
        assert payload["engine_path"] == RUNNING_ENGINE_ROOT
        assert payload["launch_attempts"] == 1
        assert "pid" not in payload
        assert harness.launched[0][0] == RUNNING_EXE
        assert harness.launched[0][1] == PROJECT_PATH
        harness.conn.send_command.assert_any_call("core.shutdown", {"force": False})
        harness.probes[0].send_command.assert_called_once_with("get_status")
        harness.probes[0].disconnect.assert_called_once()
        assert harness.probe_ports == [8743]
        harness.conn.retarget.assert_called_once_with(8743, NEW_PID, "Test")
        assert harness.conn._project_path == PROJECT_PATH
        harness.conn.disconnect.assert_called_once()
        assert not (harness.saved_dir / f"CortexPort-{OLD_PID}.txt").exists()

    def test_restart_does_not_read_ue_56_path(self, tmp_path):
        harness = RestartHarness(tmp_path)
        with patch.dict(os.environ, {"UE_56_PATH": "C:/WrongEngine"}, clear=False):
            result = harness.run(timeout=30)

        assert result["message"] == "Editor restarted successfully"
        assert "WrongEngine" not in harness.launched[0][0]

    def test_restart_force_is_passed_to_shutdown(self, tmp_path):
        harness = RestartHarness(tmp_path)

        result = harness.run(timeout=30, force=True)

        assert result["message"] == "Editor restarted successfully"
        harness.conn.send_command.assert_any_call("core.shutdown", {"force": True})

    def test_restart_unsaved_changes_refusal_launches_nothing(self, tmp_path):
        harness = RestartHarness(tmp_path)
        harness.shutdown_error = UECommandError(
            "core.shutdown",
            "UNSAVED_CHANGES",
            "1 unsaved package",
            {"dirty_packages": [{"package": "/Game/A", "file": "A.uasset"}], "count": 1},
        )

        result = harness.run(timeout=30)

        assert result["success"] is False
        assert result["_error"] == "UNSAVED_CHANGES"
        assert result["count"] == 1
        assert result["dirty_packages"][0]["package"] == "/Game/A"
        assert harness.launched == []
        harness.conn.disconnect.assert_not_called()
        assert OLD_PID in harness.alive
        assert (harness.saved_dir / f"CortexPort-{OLD_PID}.txt").exists()

    def test_restart_requires_project_path(self, tmp_path):
        conn = MagicMock(spec=UEConnection)
        conn._pid = None
        conn._project_path = None
        conn.port = 0

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False):
            mcp = MockMCP()
            register_editor_composite_tools(mcp, conn)
            result = mcp.tools["restart_editor"](timeout=10)
            payload = json.loads(result)
            assert payload["success"] is False
            assert payload["_error"] == "INVALID_OPERATION"
            assert "uproject" in payload["_message"]

    def test_restart_discovers_pid_from_port_file_of_connected_port(self, tmp_path):
        """An explicit-port connection has no pid; it is read from the port file for that port."""
        harness = RestartHarness(tmp_path, conn_pid=None)
        harness.conn._project_path = None

        result = harness.run(timeout=30)

        assert result["message"] == "Editor restarted successfully"
        assert result["old_pid"] == OLD_PID
        harness.conn.send_command.assert_any_call("core.shutdown", {"force": False})
        harness.conn.disconnect.assert_called_once()

    def test_restart_refused_before_shutdown_when_old_pid_unknown(self, tmp_path):
        """No pid on the connection and no port file for the connected port: nothing is touched."""
        harness = RestartHarness(tmp_path, conn_pid=None)
        harness.conn.port = 9999  # the only port file belongs to port 8742

        result = harness.run(timeout=30)

        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert "pid" in result["_message"]
        harness.conn.send_command.assert_not_called()
        harness.conn.disconnect.assert_not_called()
        assert harness.launched == []
        assert OLD_PID in harness.alive

    def test_restart_falls_back_to_engine_association_directory(self, tmp_path):
        engine_root = tmp_path / "InstalledEngine"
        exe = engine_root / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"
        exe.parent.mkdir(parents=True)
        exe.write_text("")
        uproject = tmp_path / "Test.uproject"
        uproject.write_text(json.dumps({"EngineAssociation": str(engine_root)}))

        harness = RestartHarness(tmp_path)
        harness.process_exe = None  # the running editor's executable cannot be queried
        harness.conn._project_path = str(uproject)

        result = harness.run(timeout=30)

        assert result["message"] == "Editor restarted successfully"
        assert result["engine_path"] == str(engine_root)
        assert harness.launched[0][0] == str(exe)

    def test_restart_refused_before_shutdown_when_engine_unresolvable(self, tmp_path):
        uproject = tmp_path / "Test.uproject"
        uproject.write_text(json.dumps({"EngineAssociation": "9.99"}))

        harness = RestartHarness(tmp_path)
        harness.process_exe = None
        harness.conn._project_path = str(uproject)

        with patch("tools.editor.composites._registry_engine_root", return_value=None):
            result = harness.run(timeout=30)

        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert "Engine" in result["_message"]
        harness.conn.send_command.assert_not_called()
        assert harness.launched == []
        assert OLD_PID in harness.alive

    def test_restart_ignores_port_files_of_other_editors(self, tmp_path):
        """A foreign editor's port file alone on the first poll must not abort the wait; the launched pid's file,
        published on a later poll, is adopted."""
        harness = RestartHarness(tmp_path)
        harness.foreign_pids = [9999]
        harness.delay_launched_file = True

        result = harness.run(timeout=30)

        assert result["message"] == "Editor restarted successfully"
        assert result["new_pid"] == NEW_PID
        assert result["port"] == 8743  # not the foreign editor's port 9999
        assert harness.probe_ports == [8743]
        assert harness.conn.port == 8743
        assert harness.conn._pid == NEW_PID

    def test_restart_verifies_through_a_probe_pinned_to_the_launched_port(self, tmp_path):
        """If the shared connection would rediscover another editor, its answer must not satisfy verification."""
        harness = RestartHarness(tmp_path, status_project="OtherProject")
        harness.rediscovered_status = {"project_name": "Test", "subsystems": {"core": True}}

        result = harness.run(timeout=30)

        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert "OtherProject" in result["_message"]
        assert harness.probe_ports == [8743]
        assert "get_status" not in [c.args[0] for c in harness.conn.send_command.call_args_list]
        harness.conn.retarget.assert_not_called()
        assert harness.conn.port == 8742  # the shared connection was not repointed

    def test_restart_fails_when_status_reports_other_pid(self, tmp_path):
        harness = RestartHarness(tmp_path)
        harness.status_pid = 4242

        result = harness.run(timeout=30)

        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert result["reported_pid"] == 4242
        harness.conn.retarget.assert_not_called()

    def test_restart_relaunches_once_when_first_launch_exits_before_ready(self, tmp_path):
        harness = RestartHarness(tmp_path)
        # The first editor publishes its port file and then dies: its stale file must not be accepted.
        harness.launch_plan = [{"pid": 4000, "exit_code": 1}, {"pid": NEW_PID}]

        result = harness.run(timeout=30)

        assert result["message"] == "Editor restarted successfully", result
        assert result["new_pid"] == NEW_PID
        assert result["launch_attempts"] == 2
        assert len(harness.launched) == 2
        assert harness.launched[0][0] == RUNNING_EXE
        assert harness.probe_ports == [8743]
        assert (harness.saved_dir / "CortexPort-4000.txt").exists()

    def test_restart_reports_no_running_editor_after_two_exited_launches(self, tmp_path):
        harness = RestartHarness(tmp_path)
        harness.launch_plan = [{"pid": 4000, "exit_code": 3}, {"pid": 4001, "exit_code": 3}]

        result = harness.run(timeout=30)

        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert result["launch"] == "exited"
        assert result["relaunch_attempted"] is True
        assert result["launch_attempts"] == 2
        assert result["editor_running"] is False
        assert result["editor_pid"] == 4001
        assert result["exit_code"] == 3
        assert "launch the editor manually" in result["_message"].lower()
        assert len(harness.launched) == 2
        harness.conn.retarget.assert_not_called()

    def test_restart_reports_still_running_editor_on_timeout_without_second_launch(self, tmp_path):
        harness = RestartHarness(tmp_path)
        harness.launch_plan = [{"pid": 4321, "writes_file": False}]

        result = harness.run(timeout=0)

        assert result["success"] is False
        assert result["launch"] == "timeout"
        assert result["relaunch_attempted"] is True
        assert result["launch_attempts"] == 1
        assert result["editor_running"] is True
        assert result["editor_pid"] == 4321
        assert "still running" in result["_message"]
        assert len(harness.launched) == 1

    def test_restart_reports_process_state_when_verification_fails(self, tmp_path):
        harness = RestartHarness(tmp_path)
        # Alive when its port file is read, gone when the pinned probe fails.
        harness.launch_plan = [{"pid": NEW_PID, "poll_seq": [None, 7]}]
        harness.probe_error = ConnectionError("refused")

        result = harness.run(timeout=30)

        assert result["success"] is False
        assert result["port"] == 8743
        assert result["pid"] == NEW_PID
        assert result["relaunch_attempted"] is True
        assert result["launch_attempts"] == 1
        assert result["editor_running"] is False
        assert result["exit_code"] == 7
        harness.conn.retarget.assert_not_called()

    def test_restart_never_launches_when_engine_is_unresolvable_and_reports_it(self, tmp_path):
        """Upstream #234: a restart that cannot relaunch leaves the editor open and says so."""
        harness = RestartHarness(tmp_path)
        harness.process_exe = None
        harness.conn._project_path = str(tmp_path / "Missing.uproject")

        result = harness.run(timeout=30)

        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert result["shutdown"] == "not_attempted"
        assert result["relaunch_attempted"] is False
        harness.conn.send_command.assert_not_called()
        assert harness.launched == []
        assert OLD_PID in harness.alive

    def test_restart_fails_when_status_reports_other_project(self, tmp_path):
        harness = RestartHarness(tmp_path, status_project="OtherProject")

        result = harness.run(timeout=30)

        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert "OtherProject" in result["_message"]


class TestDoShutdownEditor:
    """Tests for the do_shutdown_editor standalone function."""

    def test_returns_dict_on_success(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.return_value = {
            "success": True,
            "data": {"message": "Shutdown initiated", "force": True},
        }

        result = do_shutdown_editor(conn, force=True)

        assert isinstance(result, dict)
        conn.send_command.assert_called_with("core.shutdown", {"force": True})

    def test_returns_dict_on_connection_error(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.side_effect = ConnectionError("Connection lost")

        result = do_shutdown_editor(conn, force=True)

        assert isinstance(result, dict)
        assert result["message"] == "Shutdown initiated"
        assert result["force"] is True
        assert "note" in result

    def test_force_false_passed_through(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.side_effect = ConnectionError("dropped")

        result = do_shutdown_editor(conn, force=False)

        assert result["force"] is False
        conn.send_command.assert_called_with("core.shutdown", {"force": False})

    def test_force_defaults_to_false(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.return_value = {"success": True, "data": {}}

        do_shutdown_editor(conn)

        conn.send_command.assert_called_with("core.shutdown", {"force": False})


class TestDoRestartEditor:
    """Tests for the do_restart_editor standalone function."""

    def test_returns_error_dict_when_project_dir_not_set(self):
        conn = MagicMock(spec=UEConnection)

        # resolve_project_dir() has multiple fallbacks (CLAUDE_PROJECT_DIR, walk-up).
        # Mock it to return None to simulate a fully unresolvable project directory.
        with patch("tools.editor.composites.resolve_project_dir", return_value=None):
            result = do_restart_editor(conn, timeout=10)

        assert isinstance(result, dict)
        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert "project directory" in result["_message"]

    def test_returns_error_dict_when_project_path_unknown(self, tmp_path):
        conn = MagicMock(spec=UEConnection)
        conn._pid = None
        conn._project_path = None
        conn.port = 0

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False):
            result = do_restart_editor(conn, timeout=10)

        assert isinstance(result, dict)
        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert "uproject" in result["_message"]

    def test_returns_success_dict_on_full_flow(self, tmp_path):
        harness = RestartHarness(tmp_path)

        result = harness.run(timeout=30)

        assert isinstance(result, dict)
        assert result["message"] == "Editor restarted successfully"
        assert result["port"] == 8743
        assert result["new_pid"] == NEW_PID
        assert result["project"] == "Test"

    def test_cancel_between_phases_returns_cancelled(self, tmp_path):
        harness = RestartHarness(tmp_path)
        cancel = threading.Event()
        original_popen = harness._popen

        def _popen_then_cancel(args, **kwargs):
            # Signal cancellation after launch so the Phase 3 check fires
            process = original_popen(args, **kwargs)
            cancel.set()
            return process

        harness._popen = _popen_then_cancel

        result = harness.run(timeout=30, cancel=cancel)

        assert isinstance(result, dict)
        assert result.get("cancelled") is True

    def test_cancel_none_does_not_raise(self, tmp_path):
        """Passing cancel=None (default) works without error."""
        conn = MagicMock(spec=UEConnection)
        conn._pid = None
        conn._project_path = None
        conn.port = 0

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False):
            result = do_restart_editor(conn, timeout=5, cancel=None)

        assert isinstance(result, dict)
        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert result["_message"]
