"""Tests for TCP stream safety — response ID validation in _send_and_receive."""
import json
import uuid
from unittest.mock import MagicMock, patch
import pytest


def _make_response(request_id: str, success: bool = True, command: str = "test") -> bytes:
    return (json.dumps({
        "id": request_id,
        "status": "ok",
        "success": success,
        "data": {"command": command},
    }) + "\n").encode()


def _make_mismatched_response(wrong_id: str) -> bytes:
    return (json.dumps({
        "id": wrong_id,
        "status": "ok",
        "success": True,
        "data": {"stale": True},
    }) + "\n").encode()


class TestResponseIdValidation:
    """Test that _send_and_receive validates response IDs."""

    def _make_conn(self, mock_socket):
        """Create a minimal UEConnection with a mock socket."""
        from cortex_mcp.tcp_client import UEConnection
        conn = UEConnection.__new__(UEConnection)
        conn._recv_buffer = b""
        conn.host = "127.0.0.1"
        conn.port = 8742
        conn._socket = mock_socket
        conn._socket_lock = MagicMock()
        conn._socket_lock.__enter__ = MagicMock(return_value=None)
        conn._socket_lock.__exit__ = MagicMock(return_value=False)
        return conn

    def test_matching_id_reads_once(self):
        """If response ID matches request ID, only one recv call is made."""
        request_id_holder = []

        def fake_sendall(data):
            msg = json.loads(data.decode())
            request_id_holder.append(msg["id"])

        def fake_recv(n):
            rid = request_id_holder[-1] if request_id_holder else "unknown"
            return _make_response(rid)

        mock_socket = MagicMock()
        mock_socket.recv.side_effect = fake_recv
        mock_socket.sendall.side_effect = fake_sendall

        conn = self._make_conn(mock_socket)
        result = conn._send_and_receive("test_cmd", {})

        assert mock_socket.recv.call_count == 1
        assert result.get("success") is True

    def test_mismatched_id_reads_second_line(self):
        """If response ID doesn't match, reads a second line to recover."""
        request_id_holder = []

        def fake_sendall(data):
            msg = json.loads(data.decode())
            request_id_holder.append(msg["id"])

        recv_calls = [0]
        def fake_recv(n):
            recv_calls[0] += 1
            if recv_calls[0] == 1:
                # First recv: stale response with wrong ID
                return _make_mismatched_response("stale00a")
            else:
                # Second recv: correct response for the real request
                rid = request_id_holder[-1] if request_id_holder else "unknown"
                return _make_response(rid)

        mock_socket = MagicMock()
        mock_socket.recv.side_effect = fake_recv
        mock_socket.sendall.side_effect = fake_sendall

        conn = self._make_conn(mock_socket)
        result = conn._send_and_receive("test_cmd", {})

        # Should have read TWO lines (stale + correct)
        assert mock_socket.recv.call_count == 2
        # Should return the CORRECT response (not the stale one)
        assert result.get("data", {}).get("stale") is not True
        assert result.get("success") is True
