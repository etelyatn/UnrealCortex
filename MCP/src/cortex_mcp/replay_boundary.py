"""Shared pre-dispatch boundary for the five Replay operations.

Validates the exact native parameter contract before any transport call, sends
``start_replay`` exclusively through the one-shot transport, and keeps reads on
the live request path.  Native ``UECommandError`` details and the
unknown/not-dispatched outcomes follow the same deliberate ordering as the graph
one-shot boundary (``graph_patch_boundary.dispatch_graph_apply_patch``).
"""

from __future__ import annotations

import json
import re
from typing import Any

from .tcp_client import UECommandError, UECommandNotDispatchedError

# Native compact response budget (UTF-8 bytes) for one Replay reply.
MAX_REPLAY_RESPONSE_BYTES = 39000

# Human-readable diagnostics are bounded separately so that semantics-critical
# machine fields survive; the final encoded budget remains the authority.
_MAX_DIAGNOSTIC_BYTES = 4096
_TRUNCATION_MARKER = "...[truncated]"

_MAX_RECORDING_ID = 2147483647
_MIN_PAGE_SIZE = 1
_MAX_PAGE_SIZE = 100

_REPLAY_COMMANDS: dict[str, tuple[str, ...]] = {
    "list_recordings": ("after_recording_id", "page_size"),
    "get_recording": ("recording_id",),
    "start_replay": ("recording_id",),
    "get_run": ("run_id",),
    "cancel_replay": ("run_id",),
}

_CANONICAL_UUID = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
_NIL_UUID = "00000000-0000-0000-0000-000000000000"

_RESERVED_ERROR_FIELDS = {"success", "_error", "_message", "_command"}


def _encode(payload: Any) -> str:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def _limit_exceeded_response(response_bytes: int) -> str:
    """The one explicit oversize contract error; never a silent truncation."""
    return _encode(
        {
            "_error": "LIMIT_EXCEEDED",
            "_message": (
                f"Replay reply of {response_bytes} bytes exceeds the "
                f"{MAX_REPLAY_RESPONSE_BYTES}-byte MCP budget."
            ),
            "max_response_bytes": MAX_REPLAY_RESPONSE_BYTES,
            "response_bytes": response_bytes,
        }
    )


def _finalize(value: Any) -> str:
    """Enforce the encoded UTF-8 budget on EVERY result path (the single gate)."""
    text = _encode(value)
    size = len(text.encode("utf-8"))
    if size <= MAX_REPLAY_RESPONSE_BYTES:
        return text
    return _limit_exceeded_response(size)


def _cap_diagnostic(text: str, limit_bytes: int = _MAX_DIAGNOSTIC_BYTES) -> str:
    """Bound a human-readable diagnostic without splitting a UTF-8 codepoint.

    The cap is applied explicitly (a marker is appended) so machine-readable
    fields are preserved while the diagnostic never drives the whole reply over
    the encoded budget.
    """
    encoded = text.encode("utf-8")
    if len(encoded) <= limit_bytes:
        return text
    marker = _TRUNCATION_MARKER
    keep = limit_bytes - len(marker.encode("utf-8"))
    head = encoded[:keep]
    while head:
        try:
            return head.decode("utf-8") + marker
        except UnicodeDecodeError:
            head = head[:-1]
    return marker


def _error_envelope(code: str, message: str, **extra: Any) -> str:
    payload: dict[str, Any] = {"_error": code, "_message": message}
    payload.update(extra)
    return _finalize(payload)


def _strict_int(
    params: dict[str, Any],
    name: str,
    *,
    required: bool,
    minimum: int,
    maximum: int,
    message: str,
) -> tuple[int | None, str | None]:
    if name not in params:
        if required:
            return None, _error_envelope("INVALID_FIELD", f"Missing required param: {name}")
        return None, None
    value = params[name]
    if type(value) is not int or not minimum <= value <= maximum:
        return None, _error_envelope("INVALID_VALUE", message)
    return value, None


def _canonical_run_id(params: dict[str, Any]) -> tuple[str | None, str | None]:
    if "run_id" not in params:
        return None, _error_envelope("INVALID_FIELD", "Missing required param: run_id")
    value = params["run_id"]
    if not isinstance(value, str) or not _CANONICAL_UUID.fullmatch(value) or value == _NIL_UUID:
        return None, _error_envelope("INVALID_VALUE", "run_id must be a canonical UUID string")
    return value, None


def _native_error(exc: UECommandError) -> str:
    payload: dict[str, Any] = {
        "success": False,
        "_error": exc.code,
        "_message": exc.message,
        "_command": exc.command,
    }
    for key, value in exc.details.items():
        if key not in _RESERVED_ERROR_FIELDS:
            payload[key] = value
    # Never trim diagnostics: an over-budget native error is an explicit contract error.
    return _finalize(payload)


def _response_error(response: dict[str, Any]) -> str | None:
    if response.get("success") is not False:
        return None
    error = response.get("error", {})
    return _native_error(
        UECommandError(
            "replay",
            error.get("code", "UNKNOWN"),
            error.get("message", "Unknown error"),
            error.get("details", {}),
        )
    )


def _not_dispatched(exc: ConnectionError) -> str:
    return _error_envelope(
        "REPLAY_START_NOT_DISPATCHED",
        _cap_diagnostic(
            "The replay start was never dispatched and no run was admitted. " + str(exc)
        ),
        _command="replay.start_replay",
        outcome="not_dispatched",
        recovery_required=False,
    )


def _connection_error(exc: ConnectionError) -> str:
    return _error_envelope("CONNECTION_ERROR", _cap_diagnostic(str(exc)))


def _unknown_outcome(exc: ConnectionError) -> str:
    return _error_envelope(
        "REPLAY_START_OUTCOME_UNKNOWN",
        _cap_diagnostic(
            "The replay start was dispatched but its outcome is unknown. "
            "Query replay.list_recordings / replay.get_run to recover the admitted run; do not reissue start. "
            + str(exc)
        ),
        _command="replay.start_replay",
        outcome="unknown",
        recovery_required=True,
    )


def _bounded_data(data: Any) -> str:
    return _finalize(data)


def _unknown_command(command: str) -> str:
    return _error_envelope(
        "UNKNOWN_COMMAND", _cap_diagnostic(f"Unknown Replay command: {command}")
    )


def _unknown_fields(command: str, params: dict[str, Any]) -> str | None:
    allowed = _REPLAY_COMMANDS[command]
    unknown = sorted(key for key in params if key not in allowed)
    if unknown:
        return _error_envelope(
            "INVALID_FIELD",
            _cap_diagnostic(
                f"{command} accepts only its declared parameter fields; rejected: {', '.join(unknown)}"
            ),
        )
    return None


def _validated_params(command: str, params: dict[str, Any]) -> tuple[dict[str, Any] | None, str | None]:
    if command == "list_recordings":
        native: dict[str, Any] = {}
        page_size, error = _strict_int(
            params,
            "page_size",
            required=False,
            minimum=_MIN_PAGE_SIZE,
            maximum=_MAX_PAGE_SIZE,
            message="page_size must be an integer in range 1..100",
        )
        if error:
            return None, error
        if page_size is not None:
            native["page_size"] = page_size
        after, error = _strict_int(
            params,
            "after_recording_id",
            required=False,
            minimum=1,
            maximum=_MAX_RECORDING_ID,
            message="after_recording_id must be a positive signed32-bit integer",
        )
        if error:
            return None, error
        if after is not None:
            native["after_recording_id"] = after
        return native, None

    if command in ("get_recording", "start_replay"):
        recording_id, error = _strict_int(
            params,
            "recording_id",
            required=True,
            minimum=1,
            maximum=_MAX_RECORDING_ID,
            message="recording_id must be a positive signed32-bit integer",
        )
        if error:
            return None, error
        return {"recording_id": recording_id}, None

    run_id, error = _canonical_run_id(params)
    if error:
        return None, error
    return {"run_id": run_id}, None


def dispatch_replay_command(connection, command: str, params: dict | None = None) -> str:
    """Validate and dispatch one Replay command through the live native path."""
    if params is None:
        route_params: dict[str, Any] = {}
    elif isinstance(params, dict):
        route_params = params
    else:
        return _error_envelope("INVALID_FIELD", "params must be an object")
    if command not in _REPLAY_COMMANDS:
        return _unknown_command(command)

    field_error = _unknown_fields(command, route_params)
    if field_error is not None:
        return field_error

    native_params, validation_error = _validated_params(command, route_params)
    if validation_error is not None:
        return validation_error
    assert native_params is not None

    native_command = f"replay.{command}"
    if command == "start_replay":
        # One-shot semantics: never replay a start after dispatch.
        try:
            response = connection.send_command_once(native_command, native_params)
        except UECommandNotDispatchedError as exc:
            return _not_dispatched(exc)
        except UECommandError as exc:
            return _native_error(exc)
        except ConnectionError as exc:
            return _unknown_outcome(exc)
    else:
        try:
            response = connection.send_command(native_command, native_params)
        except UECommandError as exc:
            return _native_error(exc)
        except ConnectionError as exc:
            return _connection_error(exc)

    error = _response_error(response)
    if error is not None:
        return error
    return _bounded_data(response.get("data", {}))
