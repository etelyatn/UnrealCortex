"""Replay MCP boundary regressions.

These tests cover what the Python boundary itself owns: strict pre-send parameter validation,
one-shot ``start_replay`` classification (not dispatched / unknown outcome / native error), the
byte budget on every reply path, router integration and the real transport. The boundary passes
native data through unchanged, so the transport double only replays scripted answers; the
behaviour behind those answers (runs, permissions, paging) is covered by the native
``Cortex.Replay.Commands.*`` and ``Cortex.Replay.Lifecycle.*`` suites.
"""

from __future__ import annotations

import json
import socket
import threading
import uuid
from pathlib import Path

import pytest

from cortex_mcp.replay_boundary import _finalize, _limit_exceeded_response, dispatch_replay_command
from cortex_mcp.tcp_client import UECommandError, UECommandNotDispatchedError
from cortex_mcp.tools.routers import _invalid_invocation_shape, make_router, strict_router_tool

MAX_RESPONSE_BYTES = 39000
FIXTURE_PATH = Path(__file__).parent / "fixtures" / "capabilities_cache_full.json"

# A valid nonzero canonical lower-case hyphenated GUID used where a well-formed run id is needed.
RUN_ID = "abcdef01-2345-6789-abcd-ef0123456789"


def _replay_router(connection):
    return make_router("replay", connection, "replay docs")


def _core_router(connection):
    return make_router("core", connection, "core docs")


def _payload(text: str) -> dict:
    return json.loads(text)


def _error(payload: dict):
    return payload.get("_error")


def _detail(payload: dict, key: str):
    """Read a native UECommandError detail whether flattened or nested under details."""
    if key in payload:
        return payload[key]
    details = payload.get("details")
    if isinstance(details, dict):
        return details.get(key)
    return None


def _utf8_size(text: str) -> int:
    return len(text.encode("utf-8"))


def _canonical_run_id() -> str:
    return str(uuid.uuid4())


# --------------------------------------------------------------------------------------
# Scripted native-command peer
# --------------------------------------------------------------------------------------


class ScriptedPeer:
    """Transport double that answers each native command from a fixed script.

    A scripted reply is either the ``data`` of a successful response or an exception raised the
    way the real client raises it (``UECommandError`` for a native failure, ``ConnectionError``
    for transport loss). Every call is recorded so a test can assert what was, and was not, sent.
    """

    def __init__(self, replies: dict[str, dict | BaseException] | None = None) -> None:
        self.replies: dict[str, dict | BaseException] = dict(replies or {})
        self.calls: list[tuple[str, dict]] = []

    def _reply(self, command: str, params: dict | None) -> dict:
        self.calls.append((command, dict(params or {})))
        reply = self.replies[command]
        if isinstance(reply, BaseException):
            raise reply
        return {"success": True, "data": reply, "timing_ms": 1.0}

    def send_command(self, command: str, params: dict | None = None, timeout=None) -> dict:
        return self._reply(command, params)

    def send_command_once(self, command: str, params: dict | None = None, timeout=None) -> dict:
        return self._reply(command, params)

    def record_tool_invocation(self, *args, **kwargs) -> None:
        return None


@pytest.fixture
def boundary_connection() -> ScriptedPeer:
    return ScriptedPeer()


# --------------------------------------------------------------------------------------
# Strict parameter validation (fails closed before any native dispatch)
# --------------------------------------------------------------------------------------


@pytest.mark.parametrize("bad", [True, "1", 1.0, 1.5, 0, -1, 2147483648])
def test_replay_rejects_non_integer_ids_before_dispatch(boundary_connection, bad):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": bad})
    )
    assert payload["_error"] == "INVALID_VALUE"
    assert boundary_connection.calls == []


@pytest.mark.parametrize("bad", [True, "1", 1.0, 1.5, 0, -1, 2147483648])
def test_replay_get_recording_rejects_non_integer_ids(boundary_connection, bad):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "get_recording", {"recording_id": bad})
    )
    assert _error(payload) == "INVALID_VALUE"
    assert boundary_connection.calls == []


@pytest.mark.parametrize(
    "override",
    ["source", "start_pose", "guard", "tolerance", "skip_events", "wait", "speed", "path"],
)
def test_replay_rejects_start_overrides_before_send(boundary_connection, override):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": 1, override: "x"})
    )
    assert _error(payload) == "INVALID_FIELD"
    assert boundary_connection.calls == []


def test_replay_rejects_caller_supplied_human_source(boundary_connection):
    payload = _payload(
        dispatch_replay_command(
            boundary_connection, "start_replay", {"recording_id": 1, "source": "human"}
        )
    )
    assert _error(payload) == "INVALID_FIELD"


@pytest.mark.parametrize(
    "command,params",
    [
        ("get_recording", {}),
        ("start_replay", {}),
        ("get_run", {}),
        ("cancel_replay", {}),
    ],
)
def test_replay_missing_required_fields_fail_closed(command, params):
    peer = ScriptedPeer()
    payload = _payload(dispatch_replay_command(peer, command, params))
    assert _error(payload) == "INVALID_FIELD"
    assert peer.calls == []


@pytest.mark.parametrize(
    "command,params",
    [
        ("list_recordings", {"bogus": 1}),
        ("get_recording", {"recording_id": 1, "bogus": 1}),
        ("start_replay", {"recording_id": 1, "bogus": 1}),
        ("get_run", {"run_id": RUN_ID, "bogus": 1}),
        ("cancel_replay", {"run_id": RUN_ID, "bogus": 1}),
    ],
)
def test_replay_rejects_unknown_fields(command, params):
    peer = ScriptedPeer()
    payload = _payload(dispatch_replay_command(peer, command, params))
    assert _error(payload) == "INVALID_FIELD"
    assert peer.calls == []


@pytest.mark.parametrize(
    "command,base",
    [
        ("get_recording", {"recording_id": 1}),
        ("start_replay", {"recording_id": 1}),
        ("get_run", {"run_id": RUN_ID}),
        ("cancel_replay", {"run_id": RUN_ID}),
    ],
)
@pytest.mark.parametrize("field,value", [("page_size", 10), ("after_recording_id", 1)])
def test_replay_paging_fields_are_list_only(command, base, field, value):
    peer = ScriptedPeer()
    payload = _payload(dispatch_replay_command(peer, command, {**base, field: value}))
    assert _error(payload) == "INVALID_FIELD"
    assert peer.calls == []


@pytest.mark.parametrize(
    "bad",
    [
        "not-a-guid",
        "../../etc/passwd",
        "../Runs/x",
        "",
        RUN_ID.upper(),
        RUN_ID + "\n",
        "0" * 32,
        "00000000-0000-0000-0000-000000000000",
        1,
        True,
    ],
)
@pytest.mark.parametrize("command", ["get_run", "cancel_replay"])
def test_replay_rejects_noncanonical_run_ids(command, bad):
    peer = ScriptedPeer()
    payload = _payload(dispatch_replay_command(peer, command, {"run_id": bad}))
    assert _error(payload) == "INVALID_VALUE"
    assert peer.calls == []


@pytest.mark.parametrize("command", ["get_status", "start_capture", "list_human_recordings", "", "replay"])
def test_replay_unknown_command_is_rejected(command):
    peer = ScriptedPeer()
    payload = _payload(dispatch_replay_command(peer, command, {}))
    assert _error(payload) == "UNKNOWN_COMMAND"
    assert peer.calls == []


@pytest.mark.parametrize("bad", [True, False, "20", 20.0, 20.5, 0, 101, -1, 2147483648])
def test_replay_list_rejects_non_integer_page_size(boundary_connection, bad):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "list_recordings", {"page_size": bad})
    )
    assert _error(payload) == "INVALID_VALUE"
    assert boundary_connection.calls == []


@pytest.mark.parametrize("bad", [True, "1", 1.0, 1.5, 0, -1, 2147483648])
def test_replay_list_rejects_invalid_after_recording_id(boundary_connection, bad):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "list_recordings", {"after_recording_id": bad})
    )
    assert _error(payload) == "INVALID_VALUE"
    assert boundary_connection.calls == []


@pytest.mark.parametrize("field", ["limit", "cursor", "offset", "page"])
def test_replay_rejects_generic_pagination_before_any_shortcut(boundary_connection, field):
    router = _replay_router(boundary_connection)
    payload = _payload(router("list_recordings", {field: 5}))
    assert _error(payload) == "INVALID_FIELD"
    assert boundary_connection.calls == []


# --------------------------------------------------------------------------------------
# Start admission, one-shot semantics and recovery
# --------------------------------------------------------------------------------------


def test_start_replay_returns_the_native_admission_unchanged():
    admitted = {
        "run_id": RUN_ID,
        "recording_id": 1,
        "editor_instance_id": "editor-instance-1",
        "state": "Preparing",
    }
    peer = ScriptedPeer({"replay.start_replay": admitted})
    payload = _payload(dispatch_replay_command(peer, "start_replay", {"recording_id": 1}))
    assert payload == admitted
    assert peer.calls == [("replay.start_replay", {"recording_id": 1})]


@pytest.mark.parametrize("recording_id", [1, 2147483647])
def test_replay_accepts_signed32_boundary_recording_ids(recording_id):
    peer = ScriptedPeer({"replay.start_replay": {"state": "Preparing", "recording_id": recording_id}})
    payload = _payload(dispatch_replay_command(peer, "start_replay", {"recording_id": recording_id}))
    assert payload["recording_id"] == recording_id
    assert peer.calls == [("replay.start_replay", {"recording_id": recording_id})]


def test_pre_dispatch_failure_reports_not_dispatched_without_admission():
    peer = ScriptedPeer(
        {"replay.start_replay": UECommandNotDispatchedError("connection refused before one-shot dispatch")}
    )
    payload = _payload(dispatch_replay_command(peer, "start_replay", {"recording_id": 1}))
    assert _error(payload) == "REPLAY_START_NOT_DISPATCHED"
    assert payload["outcome"] == "not_dispatched"
    assert payload["recovery_required"] is False


def test_uncertain_start_reports_unknown_outcome_and_is_never_reissued():
    peer = ScriptedPeer(
        {"replay.start_replay": ConnectionError("connection closed after dispatch, acknowledgement lost")}
    )
    payload = _payload(dispatch_replay_command(peer, "start_replay", {"recording_id": 1}))
    assert _error(payload) == "REPLAY_START_OUTCOME_UNKNOWN"
    assert payload["outcome"] == "unknown"
    assert payload["recovery_required"] is True
    assert payload.get("state") is None
    # The one-shot path sends exactly once; recovery is the caller's explicit read, never a resend.
    assert [command for command, _ in peer.calls] == ["replay.start_replay"]


def test_native_command_error_preserves_structured_details():
    peer = ScriptedPeer(
        {
            "replay.get_recording": UECommandError(
                "replay.get_recording", "RECORDING_NOT_FOUND", "No recording 999", {"recording_id": 999}
            )
        }
    )
    payload = _payload(dispatch_replay_command(peer, "get_recording", {"recording_id": 999}))
    assert _error(payload) == "RECORDING_NOT_FOUND"
    assert payload["_message"]
    assert _detail(payload, "recording_id") == 999


class _OversizedErrorConnection:
    """Connection whose native error envelope exceeds the 39,000-byte budget."""

    def send_command(self, command, params=None, timeout=None):
        raise UECommandError(
            command,
            "STORAGE_FAILURE",
            "m" * 60000,
            {"blob": "x" * 60000},
        )

    def record_tool_invocation(self, *args, **kwargs):
        return None


def test_oversized_native_error_is_an_explicit_contract_error():
    result = dispatch_replay_command(_OversizedErrorConnection(), "get_recording", {"recording_id": 1})
    payload = _payload(result)
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert payload["max_response_bytes"] == MAX_RESPONSE_BYTES
    assert payload["response_bytes"] > MAX_RESPONSE_BYTES
    assert "blob" not in payload
    assert payload.get("_truncated") is not True
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES


# --------------------------------------------------------------------------------------
# CR-05: every reply path is bounded by the encoded UTF-8 budget
# --------------------------------------------------------------------------------------


def test_unknown_command_with_oversized_name_is_a_bounded_contract_error(boundary_connection):
    command = "unknown_" + "x" * 40000
    result = dispatch_replay_command(boundary_connection, command, {})
    payload = _payload(result)
    # No diagnostic pre-trimming: the complete reply overflows, so the single
    # explicit budget error is returned instead of a truncated UNKNOWN_COMMAND.
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert payload["max_response_bytes"] == MAX_RESPONSE_BYTES
    assert payload["response_bytes"] > MAX_RESPONSE_BYTES
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES
    # Pre-dispatch: the unknown command never reaches the native transport.
    assert boundary_connection.calls == []


def test_unknown_fields_with_oversized_names_are_a_bounded_contract_error(boundary_connection):
    ascii_name = "z" * 40000
    unicode_name = "\u6f22" * 40000  # 3 UTF-8 bytes per codepoint
    result = dispatch_replay_command(
        boundary_connection, "list_recordings", {ascii_name: 1, unicode_name: 2}
    )
    payload = _payload(result)
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert payload["max_response_bytes"] == MAX_RESPONSE_BYTES
    assert payload["response_bytes"] > MAX_RESPONSE_BYTES
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES
    # Pre-dispatch: rejected fields never reach the native transport.
    assert boundary_connection.calls == []


class _OversizedConnectionFailure:
    """Live read-path transport that fails with an oversized exception message."""

    def __init__(self, exc: BaseException) -> None:
        self.exc = exc
        self.calls: list[tuple[str, dict]] = []

    def send_command(self, command, params=None, timeout=None):
        self.calls.append((command, dict(params or {})))
        raise self.exc

    def record_tool_invocation(self, *args, **kwargs):
        return None


def test_long_connection_exception_is_a_bounded_contract_error():
    connection = _OversizedConnectionFailure(ConnectionError("\u6f22" * 50000))
    result = dispatch_replay_command(connection, "get_run", {"run_id": RUN_ID})
    payload = _payload(result)
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert payload["max_response_bytes"] == MAX_RESPONSE_BYTES
    assert payload["response_bytes"] > MAX_RESPONSE_BYTES
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES
    assert connection.calls != []


class _OversizedStartLossConnection:
    """One-shot start transport that dispatches, then loses the ack with oversized text."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, dict]] = []
        self.dispatched: list[dict] = []

    def send_command_once(self, command, params=None, timeout=None):
        self.calls.append((command, dict(params or {})))
        self.dispatched.append(dict(params or {}))
        raise ConnectionError("\u6f22" * 50000)

    def record_tool_invocation(self, *args, **kwargs):
        return None


def test_oversized_start_loss_keeps_unknown_outcome_and_recovery_required():
    connection = _OversizedStartLossConnection()
    result = dispatch_replay_command(connection, "start_replay", {"recording_id": 1})
    payload = _payload(result)
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert payload["max_response_bytes"] == MAX_RESPONSE_BYTES
    assert payload["response_bytes"] > MAX_RESPONSE_BYTES
    # One-shot machine context survives the overflow.
    assert payload["_command"] == "replay.start_replay"
    assert payload["outcome"] == "unknown"
    assert payload["recovery_required"] is True
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES
    # An uncertain dispatched start must never be presented as safe to retry.
    assert connection.dispatched == [{"recording_id": 1}]


class _OversizedStartNotDispatchedConnection:
    """One-shot start transport that fails before dispatch with oversized text."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, dict]] = []

    def send_command_once(self, command, params=None, timeout=None):
        self.calls.append((command, dict(params or {})))
        raise UECommandNotDispatchedError("\u6f22" * 50000)

    def record_tool_invocation(self, *args, **kwargs):
        return None


def test_oversized_start_presend_failure_keeps_not_dispatched_semantics():
    connection = _OversizedStartNotDispatchedConnection()
    result = dispatch_replay_command(connection, "start_replay", {"recording_id": 1})
    payload = _payload(result)
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert payload["max_response_bytes"] == MAX_RESPONSE_BYTES
    assert payload["response_bytes"] > MAX_RESPONSE_BYTES
    assert payload["_command"] == "replay.start_replay"
    assert payload["outcome"] == "not_dispatched"
    assert payload["recovery_required"] is False
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES


class _SurrogateStartLossConnection:
    """One-shot start transport that dispatches, then loses the ack with lone surrogates."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, dict]] = []
        self.dispatched: list[dict] = []

    def send_command_once(self, command, params=None, timeout=None):
        self.calls.append((command, dict(params or {})))
        self.dispatched.append(dict(params or {}))
        raise ConnectionError("\ud800" * 32)

    def record_tool_invocation(self, *args, **kwargs):
        return None


def test_unpaired_surrogate_local_diagnostic_never_escapes(boundary_connection):
    # A lone surrogate cannot be strict-UTF-8 encoded; the boundary must escape it
    # rather than let UnicodeEncodeError escape through the generic router.
    command = "unknown_" + "\ud800" * 64
    result = dispatch_replay_command(boundary_connection, command, {})
    payload = _payload(result)
    assert _error(payload) == "UNKNOWN_COMMAND"
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES
    assert boundary_connection.calls == []


def test_unpaired_surrogate_unknown_start_keeps_outcome_semantics():
    connection = _SurrogateStartLossConnection()
    result = dispatch_replay_command(connection, "start_replay", {"recording_id": 1})
    payload = _payload(result)
    assert _error(payload) == "REPLAY_START_OUTCOME_UNKNOWN"
    assert payload["_command"] == "replay.start_replay"
    assert payload["outcome"] == "unknown"
    assert payload["recovery_required"] is True
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES
    assert connection.dispatched == [{"recording_id": 1}]


class _OversizedSurrogateStartLossConnection:
    """One-shot start transport that dispatches, then loses the ack with huge surrogate text."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, dict]] = []
        self.dispatched: list[dict] = []

    def send_command_once(self, command, params=None, timeout=None):
        self.calls.append((command, dict(params or {})))
        self.dispatched.append(dict(params or {}))
        raise ConnectionError("\ud800" * 60000)

    def record_tool_invocation(self, *args, **kwargs):
        return None


def test_oversized_unpaired_surrogate_start_stays_unknown_and_bounded():
    connection = _OversizedSurrogateStartLossConnection()
    result = dispatch_replay_command(connection, "start_replay", {"recording_id": 1})
    payload = _payload(result)
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert payload["outcome"] == "unknown"
    assert payload["recovery_required"] is True
    assert payload["_command"] == "replay.start_replay"
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES
    assert connection.dispatched == [{"recording_id": 1}]


def test_finalize_is_total_for_surrogate_inputs_and_budget_fields_are_not_overridable():
    # Unpaired surrogate: ASCII escaping keeps the reply valid and encodable.
    escaped = _finalize({"value": "\ud800"})
    json.loads(escaped)
    assert _utf8_size(escaped) <= MAX_RESPONSE_BYTES

    # Overflow may never let preserved fields override the budget contract fields.
    hostile = _payload(
        _limit_exceeded_response(
            50000,
            {"_error": "OK", "_message": "nope", "max_response_bytes": 1, "response_bytes": 1, "outcome": "unknown"},
        )
    )
    assert hostile["_error"] == "LIMIT_EXCEEDED"
    assert hostile["max_response_bytes"] == MAX_RESPONSE_BYTES
    assert hostile["response_bytes"] == 50000
    assert hostile["outcome"] == "unknown"


def test_envelope_overflow_reports_the_measured_envelope_size():
    # A quote is escaped once in the payload JSON and again inside the SDK text envelope, so a
    # payload that fits the budget can still overflow once wrapped.
    value = {"text": '"' * 19000}
    payload_size = _utf8_size(json.dumps(value, ensure_ascii=False, separators=(",", ":")))
    assert payload_size <= MAX_RESPONSE_BYTES

    payload = _payload(_finalize(value))
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert payload["max_response_bytes"] == MAX_RESPONSE_BYTES
    assert payload["response_bytes"] > MAX_RESPONSE_BYTES
    assert payload["response_bytes"] > payload_size
    assert str(payload["response_bytes"]) in payload["_message"]


def test_sdk_envelope_keeps_non_ascii_raw_as_native_paging_assumes():
    # Native list_recordings paging (CortexReplayCommandHandler.cpp ReplayHandlerByteCost) budgets
    # the SDK text envelope as payload + one byte per quote/backslash + a fixed wrapper, i.e. it
    # assumes the SDK keeps non-ASCII text as raw UTF-8. If an SDK upgrade switched to \uXXXX
    # escaping, a natively fitting CJK page would overflow with no cursor to page past; this pins it.
    from cortex_mcp.replay_boundary import _sdk_result_bytes

    text = json.dumps({"name": "\u6f22\u00e9\U0001F600" * 20}, ensure_ascii=False, separators=(",", ":"))
    escape_growth = text.count('"') + text.count("\\")
    assert _sdk_result_bytes(text) <= _utf8_size(text) + escape_growth + 64

def test_strict_router_rejections_use_the_replay_budget(boundary_connection):
    replay_cmd = strict_router_tool(
        make_router("replay", boundary_connection, "replay docs"), "replay"
    )
    # A malformed/huge command name still yields valid, bounded JSON, never a raise.
    huge_command = "x" * 40000
    oversized = replay_cmd(huge_command, {})
    payload = _payload(oversized)
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert payload["max_response_bytes"] == MAX_RESPONSE_BYTES
    assert payload["response_bytes"] > MAX_RESPONSE_BYTES
    assert _utf8_size(oversized) <= MAX_RESPONSE_BYTES

    # The shared strict-envelope helper with the Replay finalizer is bounded too.
    envelope = _invalid_invocation_shape(
        "Malformed replay_cmd envelope: " + "y" * 40000, finalize=_finalize
    )
    envelope_payload = _payload(envelope)
    assert _error(envelope_payload) == "LIMIT_EXCEEDED"
    assert envelope_payload["response_bytes"] > MAX_RESPONSE_BYTES
    assert _utf8_size(envelope) <= MAX_RESPONSE_BYTES
    assert boundary_connection.calls == []


def test_every_replay_reply_path_stays_within_the_utf8_budget(boundary_connection):
    replies = [
        dispatch_replay_command(boundary_connection, "unknown_" + "x" * 40000, {}),
        dispatch_replay_command(
            boundary_connection, "list_recordings", {"z" * 40000: 1, "\u6f22" * 40000: 2}
        ),
        dispatch_replay_command(_OversizedErrorConnection(), "get_recording", {"recording_id": 1}),
        dispatch_replay_command(
            _OversizedConnectionFailure(ConnectionError("\u6f22" * 50000)), "get_run", {"run_id": RUN_ID}
        ),
        dispatch_replay_command(_OversizedStartLossConnection(), "start_replay", {"recording_id": 1}),
        dispatch_replay_command(
            _OversizedStartNotDispatchedConnection(), "start_replay", {"recording_id": 1}
        ),
        dispatch_replay_command(_SurrogateStartLossConnection(), "start_replay", {"recording_id": 1}),
        dispatch_replay_command(
            _OversizedSurrogateStartLossConnection(), "start_replay", {"recording_id": 1}
        ),
        dispatch_replay_command(boundary_connection, "unknown_" + "\ud800" * 64, {}),
        _invalid_invocation_shape("Malformed replay_cmd envelope: " + "y" * 40000, finalize=_finalize),
    ]
    for reply in replies:
        json.loads(reply)
        assert _utf8_size(reply) <= MAX_RESPONSE_BYTES


def test_malformed_replay_envelope_preserves_shape_error(boundary_connection):
    replay_cmd = strict_router_tool(
        make_router("replay", boundary_connection, "replay docs"), "replay"
    )
    result = replay_cmd("list_recordings", {}, unexpected_arg=1)
    payload = _payload(result)
    assert _error(payload) == "INVALID_INVOCATION_SHAPE"
    assert payload["canonical_shape"] == {"command": "string", "params": "object"}
    assert "unexpected_arg" in payload["_message"]
    assert boundary_connection.calls == []


def test_malformed_replay_envelope_with_oversized_field_is_bounded(boundary_connection):
    replay_cmd = strict_router_tool(
        make_router("replay", boundary_connection, "replay docs"), "replay"
    )
    oversized_field = "x" * 40000
    # A non-identifier top-level field reaches the strict wrapper's **_extra rejection hook.
    result = replay_cmd("list_recordings", {}, **{oversized_field: 1})
    payload = _payload(result)
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert payload["max_response_bytes"] == MAX_RESPONSE_BYTES
    assert payload["response_bytes"] > MAX_RESPONSE_BYTES
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES
    # Rejected before dispatch: no native call ever occurs.
    assert boundary_connection.calls == []


# --------------------------------------------------------------------------------------
# Oversized native replies are an explicit contract error, never a truncation
# --------------------------------------------------------------------------------------


def test_oversized_status_diagnostics_are_an_explicit_contract_error():
    peer = ScriptedPeer(
        {
            "replay.get_run": {
                "run_id": RUN_ID,
                "state": "Error",
                "execution_error": {
                    "code": "REPLAY_POSE_GUARD_FAILED",
                    "message": "m" * 60000,
                    "details": {"sequence": 0},
                },
            }
        }
    )
    result = dispatch_replay_command(peer, "get_run", {"run_id": RUN_ID})
    payload = _payload(result)
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert "execution_error" not in payload
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES


def test_oversized_native_reply_is_an_explicit_contract_error():
    # A single row far above the 39,000-byte page budget simulates a native contract violation.
    peer = ScriptedPeer(
        {"replay.list_recordings": {"recordings": [{"recording_id": 1, "description": "x" * 60000}]}}
    )
    result = dispatch_replay_command(peer, "list_recordings", {})
    payload = _payload(result)
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert "recordings" not in payload
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES


# --------------------------------------------------------------------------------------
# Router integration: core batch start ban and generic-shortcut ordering
# --------------------------------------------------------------------------------------


@pytest.mark.parametrize("command", ["batch_query", "batch"])
@pytest.mark.parametrize("key", ["commands", "steps"])
def test_core_batch_rejects_replay_start_before_send(command, key):
    peer = ScriptedPeer()
    router = _core_router(peer)
    payload = _payload(
        router(command, {key: [{"command": "replay.start_replay", "params": {"recording_id": 1}}]})
    )
    assert _error(payload) == "INVALID_OPERATION"
    assert peer.calls == []


def test_core_batch_preserves_unrelated_batches():
    peer = ScriptedPeer({"batch": {"results": [{"success": True}], "success": True}})
    router = _core_router(peer)

    reads = _payload(
        router(
            "batch_query",
            {"commands": [{"command": "replay.get_recording", "params": {"recording_id": 1}}]},
        )
    )
    assert "_error" not in reads

    unrelated = _payload(
        router("batch_query", {"commands": [{"command": "core.get_status", "params": {}}]})
    )
    assert "_error" not in unrelated
    assert [command for command, _ in peer.calls] == ["batch", "batch"]


# --------------------------------------------------------------------------------------
# Real TCP transport harness: pre-send vs post-dispatch loss
# --------------------------------------------------------------------------------------


def _tcp_read_request(conn: socket.socket, buffer: bytes):
    while b"\n" not in buffer:
        chunk = conn.recv(65536)
        if not chunk:
            return None, b""
        buffer += chunk
    line, buffer = buffer.split(b"\n", 1)
    return json.loads(line.decode("utf-8")), buffer


def _tcp_send(conn: socket.socket, message: dict) -> None:
    conn.sendall((json.dumps(message) + "\n").encode("utf-8"))


class _ScriptedReplayServer:
    """Minimal TCP peer that admits a start then drops the acknowledgement."""

    def __init__(self) -> None:
        self.admitted: list[int] = []
        self.run_id = _canonical_run_id()
        self._stop = threading.Event()
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(4)
        self.port = self._sock.getsockname()[1]
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def start(self) -> "_ScriptedReplayServer":
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        try:
            self._sock.close()
        except OSError:
            pass
        self._thread.join(timeout=3.0)

    def _serve(self) -> None:
        while not self._stop.is_set():
            try:
                self._sock.settimeout(0.5)
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                conn.settimeout(10.0)
                self._handle(conn)
            except (OSError, ValueError):
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    def _handle(self, conn: socket.socket) -> None:
        buffer = b""
        request, buffer = _tcp_read_request(conn, buffer)
        if request is None:
            return
        _tcp_send(conn, {"id": request.get("id", ""), "success": True, "data": {"domains": []}})

        request, buffer = _tcp_read_request(conn, buffer)
        if request is None:
            return
        command = request.get("command")
        if command == "replay.start_replay":
            self.admitted.append((request.get("params") or {}).get("recording_id"))
            return  # accepted natively, acknowledgement dropped
        if command == "replay.list_recordings":
            active = None
            if self.admitted:
                active = {
                    "run_id": self.run_id,
                    "recording_id": self.admitted[0],
                    "kind": "replay",
                    "origin": "ai",
                    "state": "Preparing",
                    "guard_scope": "press_only",
                    "guard_coverage": {
                        "scope": "press_only",
                        "pose_presses": 1,
                        "ui_supported_presses": 0,
                        "ui_unavailable_presses": 0,
                        "ui_not_applicable_presses": 1,
                    },
                    "dispatched_events": 0,
                    "total_events": 1,
                    "authorized_wait_seconds": 0.0,
                    "waiting": None,
                }
            _tcp_send(
                conn,
                {
                    "id": request.get("id", ""),
                    "success": True,
                    "data": {
                        "editor_instance_id": "real-tcp-editor",
                        "recordings": [],
                        "has_more": False,
                        "next_after_recording_id": None,
                        "active_ai_run": active,
                        "recent_ai_runs": [],
                        "recovery_window_seconds": 86400,
                        "recovery_max_terminal_runs": 100,
                    },
                },
            )
            return
        _tcp_send(
            conn,
            {
                "id": request.get("id", ""),
                "success": False,
                "error": {"code": "UNKNOWN_COMMAND", "message": "unexpected", "details": {}},
            },
        )


def _closed_port() -> int:
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def test_post_dispatch_connection_loss_is_unknown_outcome_over_real_transport():
    from cortex_mcp.tcp_client import UEConnection

    server = _ScriptedReplayServer().start()
    connection = UEConnection("127.0.0.1", server.port)
    try:
        payload = _payload(dispatch_replay_command(connection, "start_replay", {"recording_id": 1}))
        assert _error(payload) == "REPLAY_START_OUTCOME_UNKNOWN"
        assert payload["outcome"] == "unknown"
        assert payload["recovery_required"] is True
        assert server.admitted == [1]

        listing = _payload(dispatch_replay_command(connection, "list_recordings", {}))
        assert listing["active_ai_run"]["run_id"] == server.run_id
        assert server.admitted == [1]
    finally:
        connection.disconnect()
        server.stop()


def test_pre_dispatch_connection_refused_is_not_unknown_outcome():
    from cortex_mcp.tcp_client import UEConnection

    connection = UEConnection("127.0.0.1", _closed_port())
    payload = _payload(dispatch_replay_command(connection, "start_replay", {"recording_id": 1}))
    assert _error(payload) == "REPLAY_START_NOT_DISPATCHED"
    assert payload["outcome"] == "not_dispatched"
    assert payload["recovery_required"] is False


# --------------------------------------------------------------------------------------
# Optional capability discovery and fixture contract
# --------------------------------------------------------------------------------------


def test_replay_is_an_optional_domain_discovered_from_live_capabilities():
    from cortex_mcp.capabilities import CORE_DOMAINS, build_router_docstrings, get_registered_domains

    assert "replay" not in CORE_DOMAINS
    assert get_registered_domains(None) == CORE_DOMAINS

    cache = {"domains": {"replay": {"commands": []}}}
    assert get_registered_domains(cache) == CORE_DOMAINS + ("replay",)
    assert "replay" in build_router_docstrings(cache)


def test_capabilities_fixture_declares_replay_command_signatures():
    fixture = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
    domains = fixture["domains"]
    assert "replay" in domains

    commands = {command["name"]: command for command in domains["replay"]["commands"]}
    assert set(commands) == {"list_recordings", "get_recording", "start_replay", "get_run", "cancel_replay"}

    def signature(name):
        return [(param["name"], param["type"], param["required"]) for param in commands[name]["params"]]

    assert signature("list_recordings") == [
        ("after_recording_id", "integer", False),
        ("page_size", "integer", False),
    ]
    assert signature("get_recording") == [("recording_id", "integer", True)]
    assert signature("start_replay") == [("recording_id", "integer", True)]
    assert signature("get_run") == [("run_id", "string", True)]
    assert signature("cancel_replay") == [("run_id", "string", True)]
