"""Tests for deferred response handling in the TCP client."""

import json
import socket
import threading
import time

from cortex_mcp.tcp_client import UEConnection


def _start_mock_server(handler):
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_sock.bind(("127.0.0.1", 0))
    server_sock.listen(1)
    port = server_sock.getsockname()[1]

    thread = threading.Thread(target=handler, args=(server_sock,), daemon=True)
    thread.start()
    return server_sock, thread, port


def test_deferred_response_protocol():
    """Client should wait for final response when server acks as deferred."""
    received_request = {}

    def handler(server_sock):
        try:
            conn, _ = server_sock.accept()
        except OSError:
            return

        try:
            line = conn.recv(4096).decode("utf-8").strip()
            request = json.loads(line)
            received_request.update(request)
            req_id = request.get("id", "")

            ack = json.dumps(
                {
                    "id": req_id,
                    "status": "deferred",
                    "timeout_seconds": 30.0,
                }
            )
            conn.sendall((ack + "\n").encode("utf-8"))
            time.sleep(0.15)

            final = json.dumps(
                {
                    "id": req_id,
                    "status": "complete",
                    "success": True,
                    "data": {"state": "playing"},
                    "timing_ms": 151.0,
                }
            )
            conn.sendall((final + "\n").encode("utf-8"))
        finally:
            conn.close()
            server_sock.close()

    server_sock, server_thread, port = _start_mock_server(handler)
    try:
        conn = UEConnection("127.0.0.1", port)
        response = conn.send_command("editor.start_pie", {"mode": "selected_viewport"})
        conn.disconnect()
    finally:
        server_thread.join(timeout=3.0)
        try:
            server_sock.close()
        except OSError:
            pass

    assert response["success"] is True
    assert response["data"]["state"] == "playing"
    assert received_request["command"] == "editor.start_pie"
    assert isinstance(received_request.get("id"), str)
    assert len(received_request["id"]) > 0


def test_deferred_ack_and_final_same_packet():
    """Client should parse final response from leftover buffered data."""

    def handler(server_sock):
        try:
            conn, _ = server_sock.accept()
        except OSError:
            return

        try:
            line = conn.recv(4096).decode("utf-8").strip()
            request = json.loads(line)
            req_id = request.get("id", "")

            ack = json.dumps(
                {
                    "id": req_id,
                    "status": "deferred",
                    "timeout_seconds": 30.0,
                }
            )
            final = json.dumps(
                {
                    "id": req_id,
                    "status": "complete",
                    "success": True,
                    "data": {"state": "stopped"},
                    "timing_ms": 0.5,
                }
            )
            conn.sendall((ack + "\n" + final + "\n").encode("utf-8"))
        finally:
            conn.close()
            server_sock.close()

    server_sock, server_thread, port = _start_mock_server(handler)
    try:
        conn = UEConnection("127.0.0.1", port)
        response = conn.send_command("editor.stop_pie")
        conn.disconnect()
    finally:
        server_thread.join(timeout=3.0)
        try:
            server_sock.close()
        except OSError:
            pass

    assert response["success"] is True
    assert response["data"]["state"] == "stopped"
