"""Unit tests for editor lifecycle composite tools (shutdown, restart)."""

import json
import os
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

# Add src/ and tools/ for imports
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "editor"))

from cortex_mcp.tcp_client import UEConnection
from composites import register_editor_composite_tools


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
        port_file = saved_dir / "CortexPort.txt"

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
