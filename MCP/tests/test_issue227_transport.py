"""Issue #227: event-loop isolation, per-call timeout, and handshake request identity."""

from __future__ import annotations

import asyncio
import json
import threading

import pytest
from mcp.server.fastmcp import FastMCP

from cortex_mcp import tcp_client
from cortex_mcp.tcp_client import INVOCATION_COMMAND, UECommandTimeoutError, UEConnection
from cortex_mcp.tool_output import ToolOutputRegistrar
from cortex_mcp.tools.routers import register_router_tools


class _SizeRecorder:
    def __init__(self):
        self.recorded: list[tuple[str, str]] = []

    def record_response_size(self, tool_name, qualified_command, json_chars, utf8_bytes):
        self.recorded.append((tool_name, qualified_command))


def _text(result) -> str:
    return result.content[0].text


# --------------------------------------------------------------------------------------
# 1. Sync tools run off the event loop
# --------------------------------------------------------------------------------------


def test_blocking_sync_tool_does_not_stall_the_event_loop():
    released = threading.Event()

    async def scenario():
        mcp = FastMCP("probe")
        registrar = ToolOutputRegistrar(mcp, _SizeRecorder())

        @registrar.tool(name="blocking_probe", description="blocks until released")
        def blocking_probe() -> str:
            # Inline on the loop this waits out its full timeout: the releasing coroutine
            # below cannot run until the tool returns.
            return json.dumps({"released": released.wait(timeout=5.0)})

        async def release_soon():
            await asyncio.sleep(0.05)
            released.set()

        result, _ = await asyncio.gather(mcp.call_tool("blocking_probe", {}), release_soon())
        assert json.loads(_text(result)) == {"released": True}

    asyncio.run(scenario())


def test_routed_command_attribution_survives_the_worker_thread():
    async def scenario():
        mcp = FastMCP("probe")
        recorder = _SizeRecorder()
        registrar = ToolOutputRegistrar(mcp, recorder, metrics=True)

        @registrar.tool(name="attributed_probe", description="routes one command")
        def attributed_probe() -> str:
            INVOCATION_COMMAND.set("blueprint.compile")
            return json.dumps({"ok": True})

        INVOCATION_COMMAND.set("caller.sentinel")
        await mcp.call_tool("attributed_probe", {})
        await mcp.call_tool("attributed_probe", {})
        assert recorder.recorded == [
            ("attributed_probe", "blueprint.compile"),
            ("attributed_probe", "blueprint.compile"),
        ]
        # The worker's attribution never leaks into the caller's context.
        assert INVOCATION_COMMAND.get() == "caller.sentinel"

    asyncio.run(scenario())


class _GatedLock:
    """Real lock whose first release by one thread waits for another thread to get inside."""

    def __init__(self, gated_thread: str, inside: threading.Event):
        self._lock = threading.Lock()
        self._gated_thread = gated_thread
        self._inside = inside
        self._gated = False
        self.gated_holds = threading.Event()

    def __enter__(self):
        self._lock.acquire()
        if threading.current_thread().name == self._gated_thread:
            self.gated_holds.set()
        return self

    def __exit__(self, *exc):
        self._lock.release()
        if threading.current_thread().name == self._gated_thread and not self._gated:
            self._gated = True
            self._inside.wait(timeout=1.0)
        return False


class _FakeSocket:
    def __init__(self, role: str, sent: list, peer_done: threading.Event, inside: threading.Event):
        self.role = role
        self.closed = False
        self._sent = sent
        self._peer_done = peer_done
        self._inside = inside
        self._request_id = None

    def sendall(self, data: bytes):
        if self.role == "failing":
            raise ConnectionResetError("simulated drop")
        request = json.loads(data.decode("utf-8"))
        self._sent.append(request["command"])
        self._request_id = request["id"]
        self._inside.set()

    def settimeout(self, _value):
        pass

    def recv(self, _size):
        self._peer_done.wait(timeout=2.0)
        if self.closed:
            return b""
        return (json.dumps({"id": self._request_id, "success": True, "data": {}}) + "\n").encode()

    def close(self):
        self.closed = True


def test_a_failing_caller_never_closes_another_callers_socket():
    """A dropped connection on one thread must not tear down a request in flight on another."""
    conn = UEConnection(port=1)
    inside = threading.Event()
    failing_done = threading.Event()
    sent: list[str] = []
    conn._socket_lock = _GatedLock("failing", inside)

    def fake_connect():
        if conn._socket is None:
            role = "failing" if threading.current_thread().name == "failing" else "healthy"
            conn._socket = _FakeSocket(role, sent, failing_done, inside)

    conn.connect = fake_connect
    results: dict[str, object] = {}

    def failing():
        try:
            conn.send_command_once("level.spawn_actor", {})
        except ConnectionError as exc:
            results["failing"] = exc
        finally:
            failing_done.set()

    def healthy():
        try:
            results["healthy"] = conn.send_command("level.get_info", {})
        except ConnectionError as exc:
            results["healthy"] = exc

    t_fail = threading.Thread(target=failing, name="failing")
    t_fail.start()
    # Start the healthy caller once the failing one holds the lock.
    assert conn._socket_lock.gated_holds.wait(timeout=2.0)
    t_ok = threading.Thread(target=healthy, name="healthy")
    t_ok.start()
    t_fail.join(5)
    t_ok.join(5)

    assert isinstance(results["failing"], ConnectionError)
    assert results["healthy"] == {"id": results["healthy"]["id"], "success": True, "data": {}}
    # The healthy command went out exactly once: no replay after a foreign close.
    assert sent == ["level.get_info"]

def test_switch_editor_waits_for_a_request_in_flight(monkeypatch):
    """Retargeting must not close a socket another worker is still reading from."""
    from pathlib import Path

    from cortex_mcp.tcp_client import EditorConnection
    from cortex_mcp.tools import routers

    conn = UEConnection(port=8742)
    inside = threading.Event()
    release = threading.Event()
    sent: list[tuple[int, str]] = []

    def fake_connect():
        if conn._socket is None:
            conn._socket = _FakeSocket("healthy", [], release, inside)

    def fake_send(command, params=None, timeout=None):
        sent.append((conn.port, command))
        sock = conn._socket
        inside.set()
        release.wait(timeout=2.0)
        if sock.closed:
            conn.disconnect()
            raise ConnectionError("closed underneath")
        return {"success": True, "data": {}}

    conn.connect = fake_connect
    conn._send_and_receive = fake_send
    monkeypatch.setattr("cortex_mcp.tcp_client.time.sleep", lambda _s: None)
    target = EditorConnection(port=8743, pid=2000, started_at="t", port_file=Path("CortexPort-2000.txt"))
    monkeypatch.setattr(routers, "_discover_all_editors", lambda: [target])
    monkeypatch.setattr(routers, "_is_editor_alive", lambda _pid: True)
    router = routers.make_router("core", conn, "")

    results: dict[str, object] = {}
    worker = threading.Thread(target=lambda: results.update(cmd=conn.send_command("level.spawn_actor", {})))
    worker.start()
    assert inside.wait(timeout=2.0)
    switcher = threading.Thread(target=lambda: results.update(switch=router("switch_editor", {"pid": 2000})))
    switcher.start()
    switcher.join(timeout=0.3)
    release.set()
    worker.join(5)
    switcher.join(5)

    assert results["cmd"] == {"success": True, "data": {}}
    # Sent once, to the editor it was addressed to; the switch applied afterwards.
    assert sent == [(8742, "level.spawn_actor")]
    assert json.loads(results["switch"])["port"] == 8743
    assert conn.port == 8743 and conn._explicit_port


def test_retry_never_crosses_a_retarget(monkeypatch):
    """A switch during the reconnect delay must not send the retried command to the new editor."""
    conn = UEConnection(port=8742)
    sent: list[int] = []

    def fake_send(command, params=None, timeout=None):
        sent.append(conn.port)
        raise ConnectionError("connection reset")

    monkeypatch.setattr(conn, "connect", lambda: None)
    monkeypatch.setattr(conn, "_send_and_receive", fake_send)
    monkeypatch.setattr(
        "cortex_mcp.tcp_client.time.sleep", lambda _s: conn.retarget(8743, 2000, "OtherGame")
    )

    with pytest.raises(ConnectionError):
        conn.send_command("level.spawn_actor", {})
    assert sent == [8742]
    assert conn.port == 8743


# --------------------------------------------------------------------------------------
# 2. Per-call timeout on the router argument model
# --------------------------------------------------------------------------------------


class _TimeoutCapture(UEConnection):
    def __init__(self):
        super().__init__(port=1)
        self.calls: list[tuple[str, dict, float | None]] = []

    def connect(self):
        self._socket = object()

    def _send_and_receive(self, command, params=None, timeout=None):
        self.calls.append((command, params, timeout))
        return {"success": True, "data": {"ok": True}}


async def _router_call(arguments: dict, domain: str = "blueprint"):
    mcp = FastMCP("probe")
    connection = _TimeoutCapture()
    registrar = ToolOutputRegistrar(mcp, connection)
    register_router_tools(registrar, connection, {}, (domain,))
    result = await mcp.call_tool(f"{domain}_cmd", arguments)
    return connection, json.loads(_text(result))


def test_router_schema_exposes_optional_timeout_s():
    async def scenario():
        mcp = FastMCP("probe")
        connection = _TimeoutCapture()
        register_router_tools(ToolOutputRegistrar(mcp, connection), connection, {}, ("blueprint",))
        tool = mcp._tool_manager.get_tool("blueprint_cmd")
        assert tool.parameters["properties"]["timeout_s"]["type"] == "number"
        assert tool.parameters["required"] == ["command"]

    asyncio.run(scenario())


def test_timeout_s_sets_the_receive_window_and_is_not_forwarded():
    connection, _ = asyncio.run(
        _router_call({"command": "compile", "params": {"asset_path": "/Game/BP"}, "timeout_s": 300})
    )
    assert connection.calls == [("blueprint.compile", {"asset_path": "/Game/BP"}, 300.0)]


def test_timeout_s_replaces_a_command_default_window():
    connection, _ = asyncio.run(_router_call({"command": "compile_all", "params": {}, "timeout_s": 90}))
    assert connection.calls == [("blueprint.compile_all", {}, 90.0)]


def test_timeout_s_is_clamped_to_the_supported_range():
    high, _ = asyncio.run(_router_call({"command": "compile", "params": {}, "timeout_s": 100000}))
    huge, _ = asyncio.run(_router_call({"command": "compile", "params": {}, "timeout_s": 10**400}))
    low, _ = asyncio.run(_router_call({"command": "compile", "params": {}, "timeout_s": 0.01}))
    assert high.calls[0][2] == 3600.0
    assert huge.calls[0][2] == 3600.0
    assert low.calls[0][2] == 1.0


@pytest.mark.parametrize("value", [True, "300", -5, 0, float("nan"), float("inf"), [300]])
def test_invalid_timeout_s_is_refused_before_dispatch(value):
    connection, payload = asyncio.run(_router_call({"command": "compile", "params": {}, "timeout_s": value}))
    assert connection.calls == []
    assert payload["_error"] == "INVALID_INVOCATION_SHAPE"
    assert "timeout_s" in payload["_message"]


def test_without_timeout_s_the_default_window_is_kept():
    connection, _ = asyncio.run(_router_call({"command": "compile", "params": {}}))
    assert connection.calls == [("blueprint.compile", {}, None)]


def test_timeout_s_applies_to_one_shot_replay_dispatch():
    connection, _ = asyncio.run(
        _router_call({"command": "list_recordings", "params": {}, "timeout_s": 120}, domain="replay")
    )
    assert [timeout for _, _, timeout in connection.calls] == [120.0]


# --------------------------------------------------------------------------------------
# 3. Connect handshake goes through the identified request path
# --------------------------------------------------------------------------------------


class _HandshakeSocket:
    """Socket double: answers by request id unless the command is told to stay silent."""

    def __init__(self, project: str = "CortexSandbox", silent: frozenset[str] = frozenset()):
        self.project = project
        self.silent = silent
        self.requests: list[dict] = []
        self._pending = b""
        self.closed = False

    def settimeout(self, _value):
        pass

    def connect(self, _address):
        pass

    def sendall(self, data: bytes):
        request = json.loads(data.decode("utf-8"))
        self.requests.append(request)
        if request["command"] in self.silent:
            return
        reply = {"id": request.get("id"), "success": True, "data": {}}
        if request["command"] == "get_status":
            reply["data"] = {"project_name": self.project}
        self._pending += (json.dumps(reply) + "\n").encode()

    def recv(self, _size):
        if not self._pending:
            raise TimeoutError("timed out")
        chunk, self._pending = self._pending, b""
        return chunk

    def close(self):
        self.closed = True


def _handshake_connection(monkeypatch, sock) -> UEConnection:
    monkeypatch.setattr(tcp_client.socket, "socket", lambda *_a, **_k: sock)
    conn = UEConnection(port=1)
    conn._expected_project = "CortexSandbox"
    conn._loaded_file_cache = True
    return conn


def test_handshake_project_check_carries_a_request_id(monkeypatch):
    sock = _HandshakeSocket()
    conn = _handshake_connection(monkeypatch, sock)
    conn.connect()
    status = [r for r in sock.requests if r["command"] == "get_status"]
    assert len(status) == 1 and status[0].get("id")
    assert conn.connected


def test_unanswered_handshake_fails_the_connect_instead_of_skipping_the_check(monkeypatch):
    sock = _HandshakeSocket(silent=frozenset({"get_status"}))
    conn = _handshake_connection(monkeypatch, sock)
    with pytest.raises(ConnectionError) as raised:
        conn.connect()
    # Nothing user-visible was dispatched yet, so this must stay retryable.
    assert not isinstance(raised.value, UECommandTimeoutError)
    assert not conn.connected
    assert sock.closed


def test_handshake_project_mismatch_still_refuses(monkeypatch):
    sock = _HandshakeSocket(project="OtherGame")
    conn = _handshake_connection(monkeypatch, sock)
    with pytest.raises(ConnectionError, match="OtherGame"):
        conn.connect()
    assert not conn.connected
