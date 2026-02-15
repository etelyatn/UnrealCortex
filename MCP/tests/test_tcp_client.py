"""Unit tests for TCP client timeout handling."""

import socket
from unittest.mock import MagicMock, patch, PropertyMock

import pytest

# Ensure src is on path
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from cortex_mcp.tcp_client import UEConnection, _RECV_TIMEOUT


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

    def test_timeout_restored_after_call(self):
        """After send_and_receive, socket timeout should be restored."""
        conn = UEConnection(port=99999)
        mock_socket = MagicMock()
        mock_socket.recv.return_value = b'{"success":true,"data":{}}\n'
        mock_socket.gettimeout.return_value = 60.0
        conn._socket = mock_socket

        conn._send_and_receive("ping", timeout=120.0)

        # Last settimeout call should restore original
        last_call = mock_socket.settimeout.call_args_list[-1]
        assert last_call[0][0] == 60.0
