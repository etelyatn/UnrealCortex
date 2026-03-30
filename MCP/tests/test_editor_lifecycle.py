"""Unit tests for editor lifecycle composite tools (shutdown, restart)."""

import json
import os
import sys
from pathlib import Path
from unittest.mock import MagicMock, call, patch

# Add src/ and tools/ for imports
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "editor"))

import threading

from cortex_mcp.tcp_client import UEConnection
from composites import do_restart_editor, do_shutdown_editor, register_editor_composite_tools


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

    @patch.dict(os.environ, {"UE_56_PATH": "C:/UE_5.6"}, clear=False)
    def test_restart_full_flow(self, tmp_path):
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
            return MagicMock()

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(project_dir)}, clear=False), \
                patch("composites.psutil.pid_exists", side_effect=[True, False]), \
                patch("composites.subprocess.Popen", side_effect=_popen_side_effect), \
                patch("composites.time.sleep", return_value=None):
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
            assert "error" in payload

    @patch.dict(os.environ, {"UE_56_PATH": "C:/UE_5.6"}, clear=False)
    def test_restart_reads_project_path_from_port_file_when_pid_unknown(self, tmp_path):
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
            return MagicMock()

        # Existing port file with project_path — present before shutdown, absent after
        existing_port_file = saved_dir / "CortexPort-1234.txt"
        existing_port_file.write_text(
            '{"port":8742,"pid":1234,"project_path":"C:/test/Test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("composites.psutil.pid_exists", return_value=False), \
                patch("composites.subprocess.Popen", side_effect=_popen_side_effect), \
                patch("composites.time.sleep", return_value=None):
            mcp = MockMCP()
            register_editor_composite_tools(mcp, conn)
            result = mcp.tools["restart_editor"](timeout=30)
            payload = json.loads(result)

        assert payload.get("message") == "Editor restarted successfully", f"Unexpected result: {payload}"
        assert payload["port"] == 8743

    @patch.dict(os.environ, {"UE_56_PATH": "C:/UE_5.6"}, clear=False)
    def test_restart_shuts_down_discovered_pid_when_connection_pid_unknown(self, tmp_path):
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
            return MagicMock()

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("composites.psutil.pid_exists", side_effect=[True, False]), \
                patch("composites.subprocess.Popen", side_effect=_popen_side_effect), \
                patch("composites.time.sleep", return_value=None):
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
        with patch("composites.resolve_project_dir", return_value=None):
            result = do_restart_editor(conn, timeout=10)

        assert isinstance(result, dict)
        assert "error" in result

    def test_returns_error_dict_when_project_path_unknown(self, tmp_path):
        conn = MagicMock(spec=UEConnection)
        conn._pid = None
        conn._project_path = None

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False):
            result = do_restart_editor(conn, timeout=10)

        assert isinstance(result, dict)
        assert "error" in result

    @patch.dict(os.environ, {"UE_56_PATH": "C:/UE_5.6"}, clear=False)
    def test_returns_success_dict_on_full_flow(self, tmp_path):
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
            return MagicMock()

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=False), \
                patch("composites.psutil.pid_exists", side_effect=[True, False]), \
                patch("composites.subprocess.Popen", side_effect=_popen_side_effect), \
                patch("composites.time.sleep", return_value=None):
            result = do_restart_editor(conn, timeout=30)

        assert isinstance(result, dict)
        assert result["message"] == "Editor restarted successfully"
        assert result["port"] == 8743
        assert result["pid"] == 5678

    @patch.dict(os.environ, {"UE_56_PATH": "C:/UE_5.6"}, clear=False)
    def test_cancel_between_phases_returns_cancelled(self, tmp_path):
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
                patch("composites.subprocess.Popen", side_effect=_popen_side_effect), \
                patch("composites.time.sleep", return_value=None):
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
        assert "error" in result


class TestStartPieSessionFPSThrottle:
    """start_pie_session should inject FPS throttle commands."""

    def test_start_pie_session_sends_fps_throttle_commands(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.return_value = {
            "success": True,
            "data": {"state": "Playing", "mode": "selected_viewport"},
        }

        mcp = MockMCP()
        register_editor_composite_tools(mcp, conn)

        mcp.tools["start_pie_session"]()

        expected_calls = [
            call("editor.start_pie", {"mode": "selected_viewport"}, timeout=60.0),
            call("editor.execute_console_command", {"command": "t.MaxFPS 0"}),
            call("editor.execute_console_command", {"command": "t.UnfocusedFrameRateLimit 0"}),
            call("editor.get_pie_state"),
        ]
        conn.send_command.assert_has_calls(expected_calls)
