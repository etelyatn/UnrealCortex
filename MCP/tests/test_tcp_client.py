"""Unit tests for TCP client timeout handling."""

import socket
from unittest.mock import MagicMock, patch, PropertyMock
from pathlib import Path

import pytest

# Ensure src is on path
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from cortex_mcp.tcp_client import (
    UEConnection,
    _RECV_TIMEOUT,
    _discover_port,
    _discover_all_editors,
    _is_editor_alive,
    _parse_port_file,
    EditorConnection,
    logger as tcp_logger,
)


class TestSendCommandTimeout:
    """Tests for send_command timeout parameter."""

    def test_default_timeout_is_60s(self):
        """Default recv timeout should be 60 seconds."""
        assert _RECV_TIMEOUT == 60.0

    def test_send_command_accepts_timeout_param(self):
        """send_command should accept an optional timeout parameter."""
        conn = UEConnection(port=99999)
        # Just verify the signature accepts timeout — don't actually connect
        import inspect
        sig = inspect.signature(conn.send_command)
        assert "timeout" in sig.parameters

    def test_timeout_passed_to_socket(self):
        """When timeout is specified, socket timeout should be temporarily adjusted."""
        conn = UEConnection(port=99999)
        mock_socket = MagicMock()
        mock_socket.recv.return_value = b'{"success":true,"data":{}}\n'
        conn._socket = mock_socket

        conn._send_and_receive("ping", timeout=120.0)

        # Socket timeout should be set to 120.0 before recv
        calls = mock_socket.settimeout.call_args_list
        assert any(call[0][0] == 120.0 for call in calls), \
            f"Expected settimeout(120.0) in calls: {calls}"

    def test_timeout_uses_deadline_based_socket_timeout(self):
        """send_and_receive should apply per-read timeout from remaining deadline."""
        conn = UEConnection(port=99999)
        mock_socket = MagicMock()
        mock_socket.recv.return_value = b'{"success":true,"data":{}}\n'
        conn._socket = mock_socket

        conn._send_and_receive("ping", timeout=120.0)

        # Last settimeout call should be a positive deadline-based value.
        last_call = mock_socket.settimeout.call_args_list[-1]
        assert 0 < last_call[0][0] <= 120.0


class TestPortFileParsing:
    """Tests for JSON and plain-text port file backward compatibility."""

    def test_parse_json_port_file(self, tmp_path):
        """JSON port file should return port, pid, and project_path."""
        port_file = tmp_path / "Saved" / "CortexPort-1234.txt"
        port_file.parent.mkdir(parents=True)
        port_file.write_text(
            '{"port":8742,"pid":1234,"project":"Test","project_path":"C:/test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}), patch(
            "cortex_mcp.tcp_client._is_editor_alive", return_value=True
        ):
            result = _discover_port()
            assert result is not None
            port, pid, project_path, project_name = result
            assert port == 8742
            assert pid == 1234
            assert project_path == "C:/test.uproject"
            assert project_name == "Test"

    def test_parse_plain_text_port_file(self, tmp_path):
        """Plain integer port file should still work; PID extracted from filename."""
        port_file = tmp_path / "Saved" / "CortexPort-4321.txt"
        port_file.parent.mkdir(parents=True)
        port_file.write_text("8742")

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}), patch(
            "cortex_mcp.tcp_client._is_editor_alive", return_value=True
        ):
            result = _discover_port()
            assert result is not None
            port, pid, project_path, project_name = result
            assert port == 8742
            assert pid == 4321
            assert project_path is None
            assert project_name == ""

    def test_parse_json_port_file_without_pid_uses_filename(self, tmp_path):
        """JSON port file without 'pid' field should extract PID from filename."""
        port_file = tmp_path / "Saved" / "CortexPort-9876.txt"
        port_file.parent.mkdir(parents=True)
        port_file.write_text(
            '{"port":8742,"project_path":"C:/test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )

        result = _parse_port_file(port_file)
        assert result is not None
        assert result.port == 8742
        assert result.pid == 9876  # from filename, not JSON

    def test_parse_corrupt_port_file(self, tmp_path):
        """Corrupt port file should return None."""
        port_file = tmp_path / "Saved" / "CortexPort-9999.txt"
        port_file.parent.mkdir(parents=True)
        port_file.write_text("{corrupt json")

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}):
            result = _discover_port()
            assert result is None


class TestMultiEditorDiscovery:
    """Tests for per-PID port file discovery."""

    def test_discovers_single_pid_file(self, tmp_path):
        """Single CortexPort-{PID}.txt should be discovered."""
        saved = tmp_path / "Saved"
        saved.mkdir()
        port_file = saved / "CortexPort-1234.txt"
        port_file.write_text(
            '{"port":8742,"pid":1234,"project":"Test",'
            '"project_path":"C:/test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )
        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}), patch(
            "cortex_mcp.tcp_client._is_editor_alive", return_value=True
        ):
            result = _discover_port()
            assert result is not None
            port, pid, project_path, project_name = result
            assert port == 8742
            assert pid == 1234
            assert project_path == "C:/test.uproject"
            assert project_name == "Test"

    def test_skips_dead_pid(self, tmp_path):
        """Port files with dead PIDs should be skipped."""
        saved = tmp_path / "Saved"
        saved.mkdir()
        (saved / "CortexPort-9999.txt").write_text(
            '{"port":8742,"pid":9999,"project":"Test",'
            '"project_path":"C:/test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )
        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}), patch(
            "cortex_mcp.tcp_client._is_editor_alive", return_value=False
        ):
            result = _discover_port()
            assert result is None

    def test_selects_by_env_var(self, tmp_path):
        """CORTEX_EDITOR_PID selects matching port file."""
        saved = tmp_path / "Saved"
        saved.mkdir()
        (saved / "CortexPort-1000.txt").write_text(
            '{"port":8742,"pid":1000,"project":"Test",'
            '"project_path":"C:/test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )
        (saved / "CortexPort-2000.txt").write_text(
            '{"port":8743,"pid":2000,"project":"Test",'
            '"project_path":"C:/test.uproject","started_at":"2026-01-01T01:00:00Z"}'
        )
        with patch.dict(
            os.environ,
            {"CORTEX_PROJECT_DIR": str(tmp_path), "CORTEX_EDITOR_PID": "1000"},
        ), patch("cortex_mcp.tcp_client._is_editor_alive", return_value=True):
            result = _discover_port()
            assert result is not None
            assert result[0] == 8742
            assert result[1] == 1000
            assert result[3] == "Test"

    def test_selects_most_recent_without_env_var(self, tmp_path):
        """Without CORTEX_EDITOR_PID, selects most recent by started_at."""
        saved = tmp_path / "Saved"
        saved.mkdir()
        (saved / "CortexPort-1000.txt").write_text(
            '{"port":8742,"pid":1000,"project":"Test",'
            '"project_path":"C:/test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )
        (saved / "CortexPort-2000.txt").write_text(
            '{"port":8743,"pid":2000,"project":"Test",'
            '"project_path":"C:/test.uproject","started_at":"2026-01-01T01:00:00Z"}'
        )
        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}, clear=True), patch(
            "cortex_mcp.tcp_client._is_editor_alive", return_value=True
        ):
            result = _discover_port()
            assert result is not None
            assert result[0] == 8743
            assert result[1] == 2000
            assert result[3] == "Test"

    def test_discover_all_editors(self, tmp_path):
        """_discover_all_editors returns all live editors."""
        saved = tmp_path / "Saved"
        saved.mkdir()
        (saved / "CortexPort-1000.txt").write_text(
            '{"port":8742,"pid":1000,"project":"Test",'
            '"project_path":"C:/test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )
        (saved / "CortexPort-2000.txt").write_text(
            '{"port":8743,"pid":2000,"project":"Test",'
            '"project_path":"C:/test.uproject","started_at":"2026-01-01T01:00:00Z"}'
        )
        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}), patch(
            "cortex_mcp.tcp_client._is_editor_alive", return_value=True
        ):
            editors = _discover_all_editors()
            assert len(editors) == 2
            assert all(isinstance(e, EditorConnection) for e in editors)


class TestProjectValidation:
    """Tests for post-connect project name validation."""

    def test_project_field_in_editor_connection(self):
        """EditorConnection should carry project name."""
        ec = EditorConnection(
            port=8742, pid=1234, started_at="", port_file=Path("test.txt"), project="MyProject"
        )
        assert ec.project == "MyProject"

    def test_project_field_defaults_to_empty(self):
        """EditorConnection project should default to empty string."""
        ec = EditorConnection(
            port=8742, pid=1234, started_at="", port_file=Path("test.txt")
        )
        assert ec.project == ""

    def test_parse_port_file_reads_project(self, tmp_path):
        """_parse_port_file should read project field from JSON."""
        port_file = tmp_path / "CortexPort-1234.txt"
        port_file.write_text(
            '{"port":8742,"pid":1234,"project":"CortexSandbox",'
            '"project_path":"C:/test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )
        result = _parse_port_file(port_file)
        assert result is not None
        assert result.project == "CortexSandbox"

    def test_discover_port_populates_expected_project(self, tmp_path):
        """_discover_port should return project name from port file."""
        saved = tmp_path / "Saved"
        saved.mkdir()
        (saved / "CortexPort-1234.txt").write_text(
            '{"port":8742,"pid":1234,"project":"CortexSandbox",'
            '"project_path":"C:/test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )
        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}), patch(
            "cortex_mcp.tcp_client._is_editor_alive", return_value=True
        ):
            result = _discover_port()
            assert result is not None
            port, pid, project_path, project_name = result
            assert project_name == "CortexSandbox"

    def test_validate_project_logs_warning_on_mismatch(self):
        """_validate_project should log warning when project doesn't match."""
        conn = UEConnection(port=99999)
        conn._expected_project = "CortexSandbox"

        mock_socket = MagicMock()
        mock_socket.recv.return_value = (
            b'{"success":true,"data":{"project_name":"Ripper","plugin_version":"1.0.0"}}\n'
        )
        conn._socket = mock_socket

        with patch.object(tcp_logger, "warning") as mock_warn:
            conn._validate_project()
            mock_warn.assert_called_once()
            assert "Ripper" in mock_warn.call_args[0][1]
            assert "CortexSandbox" in mock_warn.call_args[0][2]

    def test_validate_project_no_warning_on_match(self):
        """_validate_project should not warn when projects match."""
        conn = UEConnection(port=99999)
        conn._expected_project = "CortexSandbox"

        mock_socket = MagicMock()
        mock_socket.recv.return_value = (
            b'{"success":true,"data":{"project_name":"CortexSandbox","plugin_version":"1.0.0"}}\n'
        )
        conn._socket = mock_socket

        with patch.object(tcp_logger, "warning") as mock_warn:
            conn._validate_project()
            mock_warn.assert_not_called()

    def test_validate_project_skipped_when_no_expected_project(self):
        """_validate_project should no-op if expected project is empty."""
        conn = UEConnection(port=99999)
        conn._expected_project = ""
        conn._socket = MagicMock()

        conn._validate_project()
        conn._socket.sendall.assert_not_called()

    def test_validate_project_handles_connection_error_without_disconnecting(self):
        """_validate_project should not crash or disconnect on network error."""
        conn = UEConnection(port=99999)
        conn._expected_project = "CortexSandbox"
        mock_socket = MagicMock()
        mock_socket.sendall.side_effect = BrokenPipeError("broken")
        conn._socket = mock_socket

        conn._validate_project()
        assert conn._socket is not None

    def test_connect_flow_calls_validate_with_expected_project(self):
        """Full connect() flow should call _validate_project when expected_project is set."""
        conn = UEConnection(port=99999)
        conn._expected_project = "CortexSandbox"

        with patch.object(conn, "_validate_project") as mock_validate, patch(
            "socket.socket"
        ) as mock_sock_cls, patch.object(conn, "load_file_caches"):
            mock_sock = MagicMock()
            mock_sock_cls.return_value = mock_sock
            conn.connect()
            mock_validate.assert_called_once()

    def test_connect_flow_calls_validate_even_without_expected_project(self):
        """Full connect() flow should always call _validate_project (it no-ops internally)."""
        conn = UEConnection(port=99999)
        conn._expected_project = ""

        with patch.object(conn, "_validate_project") as mock_validate, patch(
            "socket.socket"
        ) as mock_sock_cls, patch.object(conn, "load_file_caches"):
            mock_sock = MagicMock()
            mock_sock_cls.return_value = mock_sock
            conn.connect()
            mock_validate.assert_called_once()
