"""Unit tests for multi-editor switch/status MCP tooling."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

TOOLS_DIR = Path(__file__).parent.parent / "tools"
SRC_DIR = Path(__file__).parent.parent / "src"
sys.path.insert(0, str(TOOLS_DIR))
sys.path.insert(0, str(SRC_DIR))

from cortex_mcp.tcp_client import EditorConnection, UEConnection
from cortex_mcp.tools.routers import make_router


class MockMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator


def _editor(port: int, pid: int, started_at: str) -> EditorConnection:
    return EditorConnection(
        port=port,
        pid=pid,
        started_at=started_at,
        port_file=Path(f"CortexPort-{pid}.txt"),
    )


class TestSwitchEditorTool:
    def test_switch_editor_by_pid_updates_connection(self):
        conn = MagicMock(spec=UEConnection)
        conn.port = 8742
        conn._pid = 1000
        conn._project_path = "C:/Project/Test.uproject"
        router = make_router("core", conn, "core docs")

        editors = [
            _editor(8742, 1000, "2026-01-01T00:00:00Z"),
            _editor(8743, 2000, "2026-01-01T01:00:00Z"),
        ]
        with patch("cortex_mcp.tools.routers._discover_all_editors", return_value=editors), patch(
            "cortex_mcp.tools.routers._is_editor_alive", return_value=True
        ) as is_alive:
            payload = json.loads(router("switch_editor", {"pid": 2000}))

        is_alive.assert_called_once_with(2000)
        conn.disconnect.assert_called_once()
        assert conn.port == 8743
        assert conn._pid == 2000
        assert payload["pid"] == 2000
        assert payload["port"] == 8743

    def test_switch_editor_by_pid_returns_error_for_dead_pid(self):
        conn = MagicMock(spec=UEConnection)
        router = make_router("core", conn, "core docs")

        with patch("cortex_mcp.tools.routers._discover_all_editors", return_value=[_editor(8742, 1000, "2026-01-01T00:00:00Z")]), patch(
            "cortex_mcp.tools.routers._is_editor_alive", return_value=False
        ):
            payload = json.loads(router("switch_editor", {"pid": 9999}))

        assert payload["error"] == "EDITOR_NOT_FOUND"
        conn.disconnect.assert_not_called()

    def test_switch_editor_without_pid_selects_most_recent(self):
        conn = MagicMock(spec=UEConnection)
        router = make_router("core", conn, "core docs")

        editors = [
            _editor(8742, 1000, "2026-01-01T00:00:00Z"),
            _editor(8744, 3000, "2026-01-01T02:00:00Z"),
            _editor(8743, 2000, "2026-01-01T01:00:00Z"),
        ]
        with patch("cortex_mcp.tools.routers._discover_all_editors", return_value=editors):
            payload = json.loads(router("switch_editor"))

        conn.disconnect.assert_called_once()
        assert payload["pid"] == 3000
        assert payload["port"] == 8744

    def test_switch_editor_pins_deliberate_target_across_reconnect(self, monkeypatch):
        """A switched-to editor is deliberate: a reconnect must not rediscover another."""
        monkeypatch.setattr("cortex_mcp.tcp_client._discover_port", lambda: None)
        conn = UEConnection()
        assert conn._explicit_port is False

        router = make_router("core", conn, "core docs")
        editors = [
            _editor(8742, 1000, "2026-01-01T00:00:00Z"),
            _editor(8743, 2000, "2026-01-01T01:00:00Z"),
        ]
        with patch("cortex_mcp.tools.routers._discover_all_editors", return_value=editors), patch(
            "cortex_mcp.tools.routers._is_editor_alive", return_value=True
        ):
            json.loads(router("switch_editor", {"pid": 2000}))

        attempts = []

        def send(command, params=None, timeout=None):
            attempts.append(conn.port)
            if len(attempts) == 1:
                raise ConnectionError("connection reset")
            return {"success": True, "data": {}}

        monkeypatch.setattr(conn, "connect", lambda: None)
        monkeypatch.setattr(conn, "_send_and_receive", send)
        monkeypatch.setattr("cortex_mcp.tcp_client.time.sleep", lambda _seconds: None)

        def unexpected_discovery():
            raise AssertionError("a deliberate switch must stay pinned across reconnect")

        monkeypatch.setattr("cortex_mcp.tcp_client._discover_port", unexpected_discovery)

        conn.send_command("data.list_datatables", {})

        assert attempts == [8743, 8743]

    def test_switch_editor_adopts_target_project_for_validation(self):
        """Switching projects must update the validation key, not only the port."""
        conn = UEConnection(port=99999)
        conn._expected_project = "OldProject"
        router = make_router("core", conn, "core docs")
        target = EditorConnection(
            port=8743,
            pid=2000,
            started_at="2026-01-01T01:00:00Z",
            port_file=Path("CortexPort-2000.txt"),
            project="OtherGame",
        )
        with patch("cortex_mcp.tools.routers._discover_all_editors", return_value=[target]), patch(
            "cortex_mcp.tools.routers._is_editor_alive", return_value=True
        ):
            json.loads(router("switch_editor", {"pid": 2000}))

        sock = MagicMock()
        sock.recv.return_value = b'{"success":true,"data":{"project_name":"OtherGame"}}\n'
        conn._socket = sock

        conn._validate_project()

        assert conn._socket is not None

    def test_switch_editor_keeps_guard_when_target_project_is_unknown(self):
        """A legacy port file without a project must not disable the wrong-editor guard."""
        conn = UEConnection(port=99999)
        conn._expected_project = "OldProject"
        router = make_router("core", conn, "core docs")
        target = EditorConnection(
            port=8743,
            pid=2000,
            started_at="2026-01-01T01:00:00Z",
            port_file=Path("CortexPort-2000.txt"),
            project="",
        )
        with patch("cortex_mcp.tools.routers._discover_all_editors", return_value=[target]), patch(
            "cortex_mcp.tools.routers._is_editor_alive", return_value=True
        ):
            json.loads(router("switch_editor", {"pid": 2000}))

        assert conn._expected_project == "OldProject"

        sock = MagicMock()
        sock.recv.return_value = b'{"success":true,"data":{"project_name":"OtherGame"}}\n'
        conn._socket = sock

        with pytest.raises(ConnectionError, match="OtherGame.*OldProject"):
            conn._validate_project()


class TestGetStatusTool:
    def test_get_status_includes_connected_and_available_editors(self):
        from cortex_mcp import server

        conn = MagicMock()
        conn.port = 8743
        conn._pid = 2000
        conn.send_command.return_value = {
            "data": {
                "plugin_version": "1.0.0",
                "engine_version": "5.6",
                "project_name": "CortexSandbox",
            }
        }

        editors = [
            _editor(8742, 1000, "2026-01-01T00:00:00Z"),
            _editor(8743, 2000, "2026-01-01T01:00:00Z"),
        ]

        with patch.object(server, "_connection", conn), patch(
            "cortex_mcp.tcp_client._discover_all_editors", return_value=editors
        ):
            payload = json.loads(server.get_status())

        assert payload["connected_editor"] == {"pid": 2000, "port": 8743}
        assert payload["available_editors"] == [
            {"pid": 1000, "port": 8742, "started_at": "2026-01-01T00:00:00Z"},
            {"pid": 2000, "port": 8743, "started_at": "2026-01-01T01:00:00Z"},
        ]
        conn.send_command.assert_called_once_with("get_status")

    def test_get_status_returns_connection_error_string(self):
        from cortex_mcp import server

        conn = MagicMock()
        conn.send_command.side_effect = ConnectionError("boom")

        with patch.object(server, "_connection", conn):
            result = server.get_status()

        assert "Not connected to Unreal Editor" in result
