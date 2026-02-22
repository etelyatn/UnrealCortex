"""Unit tests for TCP client timeout handling."""

import socket
from unittest.mock import MagicMock, patch, PropertyMock

import pytest

# Ensure src is on path
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from cortex_mcp.tcp_client import UEConnection, _RECV_TIMEOUT, _discover_port


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
        port_file = tmp_path / "Saved" / "CortexPort.txt"
        port_file.parent.mkdir(parents=True)
        port_file.write_text(
            '{"port":8742,"pid":1234,"project":"Test","project_path":"C:/test.uproject","started_at":"2026-01-01T00:00:00Z"}'
        )

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}):
            result = _discover_port()
            assert result is not None
            port, pid, project_path = result
            assert port == 8742
            assert pid == 1234
            assert project_path == "C:/test.uproject"

    def test_parse_plain_text_port_file(self, tmp_path):
        """Plain integer port file should still work (backward compat)."""
        port_file = tmp_path / "Saved" / "CortexPort.txt"
        port_file.parent.mkdir(parents=True)
        port_file.write_text("8742")

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}):
            result = _discover_port()
            assert result is not None
            port, pid, project_path = result
            assert port == 8742
            assert pid is None
            assert project_path is None

    def test_parse_corrupt_port_file(self, tmp_path):
        """Corrupt port file should return None."""
        port_file = tmp_path / "Saved" / "CortexPort.txt"
        port_file.parent.mkdir(parents=True)
        port_file.write_text("{corrupt json")

        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(tmp_path)}):
            result = _discover_port()
            assert result is None
