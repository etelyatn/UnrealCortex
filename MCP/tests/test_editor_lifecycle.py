"""Unit tests for editor lifecycle composite tools (shutdown, restart)."""

import json
import os
import pathlib
from unittest.mock import MagicMock, patch

import threading

import pytest

from cortex_mcp.tcp_client import UEConnection
from tools.editor.composites import (
    do_restart_editor,
    do_shutdown_editor,
    register_editor_composite_tools,
)


@pytest.fixture
def fake_engine(tmp_path, monkeypatch):
    """An engine root whose UnrealEditor.exe exists, exported as UE_56_PATH."""
    engine_root = tmp_path / "Engine_Root"
    editor_exe = engine_root / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"
    editor_exe.parent.mkdir(parents=True)
    editor_exe.write_bytes(b"")
    monkeypatch.setenv("UE_56_PATH", str(engine_root))
    return editor_exe


def _editor_process(pid: int, exit_code: int | None) -> MagicMock:
    """A launched editor process stand-in: poll() is None while running, else its exit code."""
    process = MagicMock()
    process.pid = pid
    process.poll.return_value = exit_code
    return process


class MockMCP:
    """Captures tools registered via @mcp.tool()."""

    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator


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

    def test_shutdown_catches_connection_drop(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.side_effect = ConnectionError("Connection lost")

        mcp = MockMCP()
        register_editor_composite_tools(mcp, conn)

        result = mcp.tools["shutdown_editor"](force=True)
        payload = json.loads(result)
        assert payload["message"] == "Shutdown initiated"
        assert payload["force"] is True


class TestRestartEditor:
    """Tests for restart_editor composite flow."""

    def test_restart_full_flow(self, tmp_path, fake_engine):
        conn = MagicMock(spec=UEConnection)
        conn._pid = 1234
        conn._project_path = "C:/test/Test.uproject"
        conn.send_command.side_effect = [
            {"success": True, "data": {"message": "Shutdown initiated"}},
            {"success": True, "data": {"subsystems": {"core": True}}},
        ]

        project_dir = tmp_path
        saved_dir = project_dir / "Saved"
        saved_dir.mkdir(parents=True)
        port_file = saved_dir / "CortexPort-5678.txt"

        def _popen_side_effect(*args, **kwargs):
            port_file.write_text(
                '{"port":8743,"pid":5678,"project":"Test","project_path":"C:/test/Test.uproject","started_at":"2026-01-01T00:00:00Z"}'
            )
            return _editor_process(5678, exit_code=None)

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(project_dir)}, clear=False), \
                patch("tools.editor.composites.psutil.pid_exists", side_effect=[True, False]), \
                patch("tools.editor.composites.subprocess.Popen", side_effect=_popen_side_effect), \
                patch("tools.editor.composites.time.sleep", return_value=None):
            mcp = MockMCP()
            register_editor_composite_tools(mcp, conn)
            assert "restart_editor" in mcp.tools

            result = mcp.tools["restart_editor"](timeout=30)
            payload = json.loads(result)
            assert payload["message"] == "Editor restarted successfully"
            assert payload["port"] == 8743
            assert payload["pid"] == 5678

        conn.send_command.assert_any_call("core.shutdown", {"force": True})
        conn.send_command.assert_any_call("get_status")
        conn.disconnect.assert_called_once()

    def test_restart_requires_project_path(self, tmp_path):
        conn = MagicMock(spec=UEConnection)
        conn._pid = None
        conn._project_path = None

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False):
            mcp = MockMCP()
            register_editor_composite_tools(mcp, conn)
            result = mcp.tools["restart_editor"](timeout=10)
            payload = json.loads(result)
            assert payload["success"] is False
            assert payload["_error"] == "INVALID_OPERATION"
            assert "uproject" in payload["_message"]

    def test_restart_reads_project_path_from_port_file_when_pid_unknown(self, tmp_path, fake_engine):
        """project_path is read from port file when connection._pid/_project_path are None (e.g. CORTEX_PORT override)."""
        conn = MagicMock(spec=UEConnection)
        conn._pid = None
        conn._project_path = None
        conn.send_command.return_value = {"success": True, "data": {"subsystems": {"core": True}}}

        saved_dir = tmp_path / "Saved"
        saved_dir.mkdir(parents=True)

        new_port_file = saved_dir / "CortexPort-5678.txt"

        def _popen_side_effect(*args, **kwargs):
            new_port_file.write_text(
                '{"port":8743,"pid":5678,"project_path":"C:/test/Test.uproject","started_at":"2026-01-01T01:00:00Z"}'
            )
            return _editor_process(5678, exit_code=None)

        # Existing port file with project_path — present before shutdown, absent after
        existing_port_file = saved_dir / "CortexPort-1234.txt"
        existing_port_file.write_text(
            '{"port":8742,"pid":1234,"project_path":"C:/test/Test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("tools.editor.composites.psutil.pid_exists", return_value=False), \
                patch("tools.editor.composites.subprocess.Popen", side_effect=_popen_side_effect), \
                patch("tools.editor.composites.time.sleep", return_value=None):
            mcp = MockMCP()
            register_editor_composite_tools(mcp, conn)
            result = mcp.tools["restart_editor"](timeout=30)
            payload = json.loads(result)

        assert payload.get("message") == "Editor restarted successfully", f"Unexpected result: {payload}"
        assert payload["port"] == 8743

    def test_restart_shuts_down_discovered_pid_when_connection_pid_unknown(self, tmp_path, fake_engine):
        """When _pid=None but port file has a live PID, Phase 1 shuts it down via that discovered PID."""
        conn = MagicMock(spec=UEConnection)
        conn._pid = None
        conn._project_path = None
        # shutdown raises ConnectionError (expected — editor closes socket), status succeeds
        conn.send_command.side_effect = [
            ConnectionError("Connection closed"),
            {"success": True, "data": {"subsystems": {"core": True}}},
        ]

        saved_dir = tmp_path / "Saved"
        saved_dir.mkdir(parents=True)

        existing_port_file = saved_dir / "CortexPort-1234.txt"
        existing_port_file.write_text(
            '{"port":8742,"pid":1234,"project_path":"C:/test/Test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )

        new_port_file = saved_dir / "CortexPort-5678.txt"

        def _popen_side_effect(*args, **kwargs):
            existing_port_file.unlink(missing_ok=True)
            new_port_file.write_text(
                '{"port":8743,"pid":5678,"project_path":"C:/test/Test.uproject","started_at":"2026-01-01T01:00:00Z"}'
            )
            return _editor_process(5678, exit_code=None)

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("tools.editor.composites.psutil.pid_exists", side_effect=[True, False]), \
                patch("tools.editor.composites.subprocess.Popen", side_effect=_popen_side_effect), \
                patch("tools.editor.composites.time.sleep", return_value=None):
            mcp = MockMCP()
            register_editor_composite_tools(mcp, conn)
            result = mcp.tools["restart_editor"](timeout=30)
            payload = json.loads(result)

        assert payload.get("message") == "Editor restarted successfully", f"Unexpected result: {payload}"
        assert payload["port"] == 8743
        conn.send_command.assert_any_call("core.shutdown", {"force": True})
        conn.disconnect.assert_called_once()


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

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False):
            result = do_restart_editor(conn, timeout=10)

        assert isinstance(result, dict)
        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert "uproject" in result["_message"]

    def test_returns_success_dict_on_full_flow(self, tmp_path, fake_engine):
        conn = MagicMock(spec=UEConnection)
        conn._pid = 1234
        conn._project_path = "C:/test/Test.uproject"
        conn.send_command.side_effect = [
            {"success": True, "data": {"message": "Shutdown initiated"}},
            {"success": True, "data": {"subsystems": {"core": True}}},
        ]

        saved_dir = tmp_path / "Saved"
        saved_dir.mkdir(parents=True)
        port_file = saved_dir / "CortexPort-5678.txt"

        def _popen_side_effect(*args, **kwargs):
            port_file.write_text(
                '{"port":8743,"pid":5678,"project":"Test","project_path":"C:/test/Test.uproject","started_at":"2026-01-01T00:00:00Z"}'
            )
            return _editor_process(5678, exit_code=None)

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("tools.editor.composites.psutil.pid_exists", side_effect=[True, False]), \
                patch("tools.editor.composites.subprocess.Popen", side_effect=_popen_side_effect), \
                patch("tools.editor.composites.time.sleep", return_value=None):
            result = do_restart_editor(conn, timeout=30)

        assert isinstance(result, dict)
        assert result["message"] == "Editor restarted successfully"
        assert result["port"] == 8743
        assert result["pid"] == 5678

    def test_cancel_between_phases_returns_cancelled(self, tmp_path, fake_engine):
        conn = MagicMock(spec=UEConnection)
        conn._pid = None
        conn._project_path = "C:/test/Test.uproject"

        saved_dir = tmp_path / "Saved"
        saved_dir.mkdir(parents=True)

        cancel = threading.Event()

        def _popen_side_effect(*args, **kwargs):
            # Signal cancellation after launch so Phase 3 check fires
            cancel.set()
            return MagicMock()

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("tools.editor.composites.subprocess.Popen", side_effect=_popen_side_effect), \
                patch("tools.editor.composites.time.sleep", return_value=None):
            result = do_restart_editor(conn, timeout=30, cancel=cancel)

        assert isinstance(result, dict)
        assert result.get("cancelled") is True

    def test_cancel_none_does_not_raise(self, tmp_path):
        """Passing cancel=None (default) works without error."""
        conn = MagicMock(spec=UEConnection)
        conn._pid = None
        conn._project_path = None

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False):
            result = do_restart_editor(conn, timeout=5, cancel=None)

        assert isinstance(result, dict)
        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert result["_message"]


class TestRestartEditorRelaunch:
    """Issue #234: a restart never leaves the editor closed without saying so."""

    @staticmethod
    def _connection() -> MagicMock:
        conn = MagicMock(spec=UEConnection)
        conn._pid = 1234
        conn._project_path = "C:/test/Test.uproject"
        return conn

    @pytest.mark.parametrize("engine_path", [None, "missing"])
    def test_refuses_before_shutdown_when_editor_cannot_be_launched(self, tmp_path, monkeypatch, engine_path):
        if engine_path is None:
            monkeypatch.delenv("UE_56_PATH", raising=False)
        else:
            monkeypatch.setenv("UE_56_PATH", str(tmp_path / engine_path))
        (tmp_path / "Saved").mkdir()
        conn = self._connection()

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("tools.editor.composites.psutil.pid_exists", return_value=True), \
                patch("tools.editor.composites.subprocess.Popen") as popen:
            result = do_restart_editor(conn, timeout=30)

        assert result["success"] is False
        assert result["_error"] == "INVALID_OPERATION"
        assert result["shutdown"] == "not_attempted"
        assert result["relaunch_attempted"] is False
        conn.send_command.assert_not_called()
        popen.assert_not_called()

    def test_relaunches_once_when_first_launch_exits_before_ready(self, tmp_path, fake_engine):
        saved_dir = tmp_path / "Saved"
        saved_dir.mkdir()
        conn = self._connection()
        conn.send_command.side_effect = [
            ConnectionError("Connection closed"),
            {"success": True, "data": {"subsystems": {"core": True}}},
        ]
        launches = []

        def _popen(*args, **kwargs):
            launches.append(args[0])
            if len(launches) == 1:
                # The first editor publishes its port file and then dies: its stale file must not be accepted.
                (saved_dir / "CortexPort-4000.txt").write_text('{"port":8800,"pid":4000}')
                return _editor_process(4000, exit_code=1)
            (saved_dir / "CortexPort-5678.txt").write_text('{"port":8743,"pid":5678}')
            return _editor_process(5678, exit_code=None)

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("tools.editor.composites.psutil.pid_exists", side_effect=[True, False]), \
                patch("tools.editor.composites.subprocess.Popen", side_effect=_popen), \
                patch("tools.editor.composites.time.sleep", return_value=None):
            result = do_restart_editor(conn, timeout=30)

        assert result["message"] == "Editor restarted successfully", result
        assert result["pid"] == 5678
        assert result["launch_attempts"] == 2
        assert len(launches) == 2
        assert pathlib.Path(launches[0][0]) == fake_engine

    def test_reports_no_running_editor_after_two_exited_launches(self, tmp_path, fake_engine):
        (tmp_path / "Saved").mkdir()
        conn = self._connection()
        conn.send_command.side_effect = ConnectionError("Connection closed")
        processes = [_editor_process(4000, exit_code=3), _editor_process(4001, exit_code=3)]

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("tools.editor.composites.psutil.pid_exists", side_effect=[True, False]), \
                patch("tools.editor.composites.subprocess.Popen", side_effect=processes) as popen, \
                patch("tools.editor.composites.time.sleep", return_value=None):
            result = do_restart_editor(conn, timeout=30)

        assert result["success"] is False
        assert result["launch"] == "exited"
        assert result["relaunch_attempted"] is True
        assert result["launch_attempts"] == 2
        assert result["editor_running"] is False
        assert result["editor_pid"] == 4001
        assert result["exit_code"] == 3
        assert "launch the editor manually" in result["_message"].lower()
        assert popen.call_count == 2

    def test_reports_still_running_editor_on_timeout_without_second_launch(self, tmp_path, fake_engine):
        (tmp_path / "Saved").mkdir()
        conn = self._connection()
        conn._pid = None

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("tools.editor.composites.subprocess.Popen",
                      return_value=_editor_process(4321, exit_code=None)) as popen:
            result = do_restart_editor(conn, timeout=0)

        assert result["success"] is False
        assert result["launch"] == "timeout"
        assert result["relaunch_attempted"] is True
        assert result["launch_attempts"] == 1
        assert result["editor_running"] is True
        assert result["editor_pid"] == 4321
        assert "still running" in result["_message"]
        assert popen.call_count == 1

    def test_reports_process_state_when_verification_fails(self, tmp_path, fake_engine):
        saved_dir = tmp_path / "Saved"
        saved_dir.mkdir()
        conn = self._connection()
        conn._pid = None
        conn.send_command.side_effect = ConnectionError("refused")
        process = _editor_process(5678, exit_code=None)
        process.poll.side_effect = [None, 7]  # alive when its port file is read, gone when get_status fails

        def _popen(*args, **kwargs):
            (saved_dir / "CortexPort-5678.txt").write_text('{"port":8743,"pid":5678}')
            return process

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("tools.editor.composites.subprocess.Popen", side_effect=_popen), \
                patch("tools.editor.composites.time.sleep", return_value=None):
            result = do_restart_editor(conn, timeout=30)

        assert result["success"] is False
        assert result["pid"] == 5678
        assert result["relaunch_attempted"] is True
        assert result["launch_attempts"] == 1
        assert result["editor_running"] is False
        assert result["exit_code"] == 7
