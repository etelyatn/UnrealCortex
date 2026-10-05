"""Task 7 MCP replay one-shot/live boundary regressions.

Phase A deliverable: these tests pin the Replay MCP boundary contract before
``cortex_mcp.replay_boundary`` exists.  They exercise public output, recovery
semantics and pre-send fail-closed behaviour through a stateful native-command
peer (accepted run state plus a dropped acknowledgement), not a mock that echoes
arguments.  Pytest collection is lazy for the not-yet-implemented boundary so the
file fails red on the missing module rather than on import.
"""

from __future__ import annotations

import json
import socket
import threading
import uuid
from pathlib import Path

import pytest

from cortex_mcp.tcp_client import UECommandError, UECommandNotDispatchedError

MAX_RESPONSE_BYTES = 39000
FIXTURE_PATH = Path(__file__).parent / "fixtures" / "capabilities_cache_full.json"

# A valid nonzero canonical lower-case hyphenated GUID used where a well-formed run id is needed.
RUN_ID = "abcdef01-2345-6789-abcd-ef0123456789"

TERMINAL_STATES = {"Completed", "Cancelled", "Interrupted", "Error"}


# --------------------------------------------------------------------------------------
# Boundary accessors (lazy, so a missing Phase B module is a red test not a collection error)
# --------------------------------------------------------------------------------------


def dispatch_replay_command(connection, command: str, params: dict | None = None) -> str:
    from importlib import import_module

    module = import_module("cortex_mcp.replay_boundary")
    return module.dispatch_replay_command(connection, command, params)


def _replay_router(connection):
    from cortex_mcp.tools.routers import make_router

    return make_router("replay", connection, "replay docs")


def _core_router(connection):
    from cortex_mcp.tools.routers import make_router

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


def _worst_case_text(length: int) -> str:
    # Mirrors the native worst-case text: JSON-escaped ASCII plus 3-byte BMP Unicode.
    pattern = ["\\", '"', "\u6f22"]
    return "".join(pattern[index % 3] for index in range(length))


def _start(peer: "ReplayCommandPeer", recording_id: int = 1) -> str:
    payload = _payload(dispatch_replay_command(peer, "start_replay", {"recording_id": recording_id}))
    assert payload.get("state") == "Preparing", payload
    return payload["run_id"]


# --------------------------------------------------------------------------------------
# Stateful deterministic native-command peer
# --------------------------------------------------------------------------------------


class ReplayCommandPeer:
    """A deterministic native command peer that owns a library and a run store.

    It models accepted run state and dropped acknowledgements so the boundary's
    one-shot/retry/recovery behaviour can be observed through native state, never
    through mock call arguments.  Native failures raise ``UECommandError`` exactly
    like the real TCP client does for a ``success:false`` envelope.
    """

    def __init__(self) -> None:
        self.recordings: dict[int, dict] = {}
        self.runs: dict[str, dict] = {}
        self.active_run_id: str | None = None
        self.started_run_ids: list[str] = []
        self.native_start_count = 0
        self.calls: list[tuple[str, dict]] = []
        self.editor_instance_id = "editor-instance-1"
        self.drop_next_start_ack = False
        self.fail_next_start_presend = False

    # -- library / run state helpers ---------------------------------------------------

    def add_recording(
        self,
        recording_id: int,
        *,
        ai_enabled: bool = True,
        name: str | None = None,
        description: str = "",
        map_asset_path: str = "/Game/Maps/TestMap",
        initial_state_sha256: str = "a" * 64,
        inputs_sha256: str = "b" * 64,
        recording_snapshot_sha256: str = "c" * 64,
        coverage: dict | None = None,
    ) -> int:
        self.recordings[recording_id] = {
            "format": "CortexReplay",
            "schema_version": 1,
            "recording_id": recording_id,
            "name": name if name is not None else f"Recording {recording_id}",
            "description": description,
            "map_asset_path": map_asset_path,
            "engine_version": "5.8.0",
            "plugin_version": "0.5.0",
            "created_at_utc": "2026-10-05T00:00:00.000Z",
            "duration_seconds": 1.5,
            "ai_enabled": ai_enabled,
            "complete": True,
            "initial_state_sha256": initial_state_sha256,
            "inputs_sha256": inputs_sha256,
            "recording_snapshot_sha256": recording_snapshot_sha256,
            "prerequisites": {
                "local_player_index": 0,
                "viewport_size": {"x": 1280, "y": 720},
                "dpi_scale": 1.0,
                "input_device": "keyboard_mouse",
            },
            "guard_coverage": coverage
            or {
                "scope": "press_only",
                "pose_presses": 2,
                "ui_supported_presses": 1,
                "ui_unavailable_presses": 1,
                "ui_not_applicable_presses": 0,
            },
            "initial_state": {
                "schema_version": 1,
                "recording_id": recording_id,
                "pawn_class_path": "/Script/Engine.DefaultPawn",
                "pawn_transform": {
                    "location_cm": {"x": 0.0, "y": 0.0, "z": 100.0},
                    "rotation_deg": {"pitch": 0.0, "yaw": 0.0, "roll": 0.0},
                    "scale": {"x": 1.0, "y": 1.0, "z": 1.0},
                },
                "control_rotation_deg": {"pitch": 0.0, "yaw": 0.0, "roll": 0.0},
            },
        }
        return recording_id

    def revoke(self, recording_id: int) -> None:
        """Commit a permission revocation; a live run for that recording cancels natively."""
        self.recordings[recording_id]["ai_enabled"] = False
        for run_id, run in self.runs.items():
            if (
                run["origin"] == "ai"
                and run["recording_id"] == recording_id
                and run["state"] not in TERMINAL_STATES
            ):
                run["state"] = "Cancelled"
                run["waiting"] = None
                if run["finalized_at_utc"] is None:
                    run["finalized_at_utc"] = "2026-10-05T00:05:00.000Z"
                if self.active_run_id == run_id:
                    self.active_run_id = None

    def delete_recording(self, recording_id: int) -> None:
        self.recordings.pop(recording_id, None)

    def replace_library(self, recordings: list[dict]) -> None:
        self.recordings = {int(record["recording_id"]): dict(record) for record in recordings}

    def restart(self) -> None:
        """Editor/service restart: active state drops, retained terminal runs survive."""
        self.active_run_id = None

    def advance_run(self, run_id: str, **fields) -> None:
        self.runs[run_id].update(fields)

    def add_terminal_run(
        self,
        *,
        run_id: str | None = None,
        recording_id: int = 1,
        state: str = "Cancelled",
        origin: str = "ai",
        started_at_utc: str = "2026-10-05T00:00:00.000Z",
        finalized_at_utc: str = "2026-10-05T01:00:00.000Z",
    ) -> str:
        run_id = run_id or _canonical_run_id()
        self.runs[run_id] = self._run_record(
            run_id,
            recording_id,
            origin=origin,
            state=state,
            started_at_utc=started_at_utc,
            finalized_at_utc=finalized_at_utc,
        )
        return run_id

    # -- native command transport ------------------------------------------------------

    def send_command(self, command: str, params: dict | None = None, timeout=None) -> dict:
        self.calls.append((command, dict(params or {})))
        return {"success": True, "data": self._dispatch(command, params or {}), "timing_ms": 1.0}

    def send_command_once(self, command: str, params: dict | None = None, timeout=None) -> dict:
        if self.fail_next_start_presend:
            self.fail_next_start_presend = False
            raise UECommandNotDispatchedError("connection refused before one-shot dispatch")
        self.calls.append((command, dict(params or {})))
        data = self._dispatch(command, params or {})
        if self.drop_next_start_ack:
            self.drop_next_start_ack = False
            raise ConnectionError("connection closed after dispatch, acknowledgement lost")
        return {"success": True, "data": data, "timing_ms": 1.0}

    def send_command_cached(self, command: str, params: dict | None = None, ttl: float = 300.0, timeout=None) -> dict:
        return self.send_command(command, params, timeout)

    def record_tool_invocation(self, *args, **kwargs) -> None:
        return None

    # -- native dispatch ----------------------------------------------------------------

    def _dispatch(self, command: str, params: dict) -> dict:
        name = command.split(".", 1)[1] if "." in command else command
        if name == "list_recordings":
            return self._list(params)
        if name == "get_recording":
            return dict(self._recording_or_error(params, "replay.get_recording"))
        if name == "start_replay":
            return self._start(params)
        if name == "get_run":
            return self._get_run(params)
        if name == "cancel_replay":
            return self._cancel(params)
        if name == "batch":
            return self._batch(params)
        if name == "get_status":
            return {"subsystems": {"core": True}}
        raise UECommandError(command, "UNKNOWN_COMMAND", f"Unknown command: {command}", {})

    def _run_record(
        self,
        run_id: str,
        recording_id: int,
        *,
        origin: str = "ai",
        state: str = "Preparing",
        started_at_utc: str = "2026-10-05T00:00:00.000Z",
        finalized_at_utc: str | None = None,
    ) -> dict:
        record = self.recordings.get(recording_id, {})
        coverage = record.get(
            "guard_coverage",
            {
                "scope": "press_only",
                "pose_presses": 0,
                "ui_supported_presses": 0,
                "ui_unavailable_presses": 0,
                "ui_not_applicable_presses": 0,
            },
        )
        return {
            "run_id": run_id,
            "recording_id": recording_id,
            "origin": origin,
            "editor_instance_id": self.editor_instance_id,
            "state": state,
            "guard_scope": "press_only",
            "guard_coverage": dict(coverage),
            "dispatched_events": 0,
            "total_events": coverage.get("pose_presses", 0),
            "authorized_wait_seconds": 0.0,
            "recording_snapshot_sha256": record.get("recording_snapshot_sha256", ""),
            "initial_state_sha256": record.get("initial_state_sha256", ""),
            "inputs_sha256": record.get("inputs_sha256", ""),
            "started_at_utc": started_at_utc,
            "finalized_at_utc": finalized_at_utc,
            "execution_error": None,
            "waiting": None,
        }

    def _recording_or_error(self, params: dict, command: str) -> dict:
        recording_id = params.get("recording_id")
        record = self.recordings.get(recording_id)
        if record is None:
            raise UECommandError(
                command,
                "RECORDING_NOT_FOUND",
                f"Recording {recording_id} was not found",
                {"recording_id": recording_id},
            )
        if not record["ai_enabled"]:
            raise UECommandError(
                command,
                "PERMISSION_DENIED",
                "Recording is not enabled for AI replay",
                {"recording_id": recording_id},
            )
        return record

    def _start(self, params: dict) -> dict:
        record = self._recording_or_error(params, "replay.start_replay")
        run_id = _canonical_run_id()
        self.active_run_id = run_id
        self.started_run_ids.append(run_id)
        self.native_start_count += 1
        self.runs[run_id] = self._run_record(run_id, record["recording_id"])
        return {
            "run_id": run_id,
            "recording_id": record["recording_id"],
            "editor_instance_id": self.editor_instance_id,
            "state": "Preparing",
        }

    def _get_run(self, params: dict) -> dict:
        run_id = params.get("run_id")
        run = self.runs.get(run_id)
        if run is None or run["origin"] != "ai":
            raise UECommandError(
                "replay.get_run", "RUN_NOT_FOUND", f"Run {run_id} was not found", {"run_id": run_id}
            )
        return dict(run)

    def _cancel(self, params: dict) -> dict:
        run_id = params.get("run_id")
        run = self.runs.get(run_id)
        if run is None or run["origin"] != "ai":
            raise UECommandError(
                "replay.cancel_replay", "RUN_NOT_FOUND", f"Run {run_id} was not found", {"run_id": run_id}
            )
        if run["state"] not in TERMINAL_STATES:
            run["state"] = "Cancelled"
        if run["finalized_at_utc"] is None:
            run["finalized_at_utc"] = "2026-10-05T00:05:00.000Z"
        run["waiting"] = None
        if self.active_run_id == run_id:
            self.active_run_id = None
        return dict(run)

    def _row(self, recording_id: int) -> dict:
        return {key: value for key, value in self.recordings[recording_id].items() if key != "initial_state"}

    def _active_summary(self) -> dict | None:
        run = self.runs.get(self.active_run_id) if self.active_run_id else None
        if run is None or run["state"] in TERMINAL_STATES:
            return None
        return {
            "kind": "replay",
            "origin": run["origin"],
            "recording_id": run["recording_id"],
            "run_id": run["run_id"],
            "state": run["state"],
            "guard_coverage": run["guard_coverage"],
            "dispatched_events": run["dispatched_events"],
            "total_events": run["total_events"],
            "authorized_wait_seconds": run["authorized_wait_seconds"],
            "waiting": run["waiting"],
        }

    def _recent_summaries(self) -> list[dict]:
        terminal = [run for run in self.runs.values() if run["origin"] == "ai" and run["state"] in TERMINAL_STATES]
        terminal.sort(key=lambda run: (run["finalized_at_utc"] or "", run["run_id"]), reverse=True)
        keys = (
            "run_id",
            "recording_id",
            "editor_instance_id",
            "started_at_utc",
            "finalized_at_utc",
            "state",
        )
        return [{key: run[key] for key in keys} for run in terminal[:100]]

    def _list(self, params: dict) -> dict:
        after = params.get("after_recording_id")
        after = 0 if after is None else after
        page_size = params.get("page_size", 20)
        eligible = sorted(
            recording_id
            for recording_id, record in self.recordings.items()
            if record["ai_enabled"] and recording_id > after
        )
        selected = eligible[:page_size]
        has_more = len(eligible) > len(selected)
        data = {
            "editor_instance_id": self.editor_instance_id,
            "recordings": [self._row(recording_id) for recording_id in selected],
            "has_more": has_more,
            "next_after_recording_id": selected[-1] if (has_more and selected) else None,
            "active_ai_run": self._active_summary(),
            "recent_ai_runs": self._recent_summaries(),
            "recovery_window_seconds": 86400,
            "recovery_max_terminal_runs": 100,
        }
        return self._apply_page_budget(data)

    def _apply_page_budget(self, data: dict) -> dict:
        rows = data["recordings"]
        if not rows:
            return data
        reserve = 16
        fixed = {key: value for key, value in data.items() if key != "recordings"}
        fixed["recordings"] = []
        fixed_bytes = _utf8_size(json.dumps(fixed, ensure_ascii=False, separators=(",", ":")))
        included: list[dict] = []
        summed = 0
        for row in rows:
            row_bytes = _utf8_size(json.dumps(row, ensure_ascii=False, separators=(",", ":")))
            candidate = fixed_bytes + summed + row_bytes + len(included)
            if included and candidate > MAX_RESPONSE_BYTES - reserve:
                break
            included.append(row)
            summed += row_bytes
        data["recordings"] = included
        if len(included) < len(rows):
            data["has_more"] = True
            data["next_after_recording_id"] = included[-1]["recording_id"]
        return data

    def _batch(self, params: dict) -> dict:
        steps = params.get("commands")
        if steps is None:
            steps = params.get("steps")
        results = []
        for step in steps or []:
            step = dict(step)
            try:
                results.append({"success": True, "data": self._dispatch(step.get("command"), step.get("params") or {})})
            except UECommandError as exc:
                results.append(
                    {
                        "success": False,
                        "error_code": exc.code,
                        "error": {"code": exc.code, "message": exc.message, "details": exc.details},
                    }
                )
        return {"results": results, "success": all(result["success"] for result in results)}


@pytest.fixture
def boundary_connection() -> ReplayCommandPeer:
    peer = ReplayCommandPeer()
    peer.add_recording(1, description="Enabled recording")
    peer.add_recording(2, ai_enabled=False, description="Disabled recording")
    return peer


# --------------------------------------------------------------------------------------
# Strict parameter validation (fails closed before any native dispatch)
# --------------------------------------------------------------------------------------


@pytest.mark.parametrize("bad", [True, "1", 1.0, 1.5, 0, -1, 2147483648])
def test_replay_rejects_non_integer_ids_before_dispatch(boundary_connection, bad):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": bad})
    )
    assert payload["_error"] == "INVALID_VALUE"
    assert boundary_connection.started_run_ids == []
    assert boundary_connection.calls == []


@pytest.mark.parametrize("bad", [True, "1", 1.0, 1.5, 0, -1, 2147483648])
def test_replay_get_recording_rejects_non_integer_ids(boundary_connection, bad):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "get_recording", {"recording_id": bad})
    )
    assert _error(payload) == "INVALID_VALUE"
    assert boundary_connection.calls == []


def test_replay_accepts_signed32_boundary_recording_ids(boundary_connection):
    boundary_connection.add_recording(2147483647)
    for recording_id in (1, 2147483647):
        payload = _payload(
            dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": recording_id})
        )
        assert payload.get("state") == "Preparing"
        assert payload["recording_id"] == recording_id


@pytest.mark.parametrize(
    "override",
    ["source", "start_pose", "guard", "tolerance", "skip_events", "wait", "speed", "path"],
)
def test_replay_rejects_start_overrides_before_send(boundary_connection, override):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": 1, override: "x"})
    )
    assert _error(payload) == "INVALID_FIELD"
    assert boundary_connection.started_run_ids == []
    assert boundary_connection.calls == []


def test_replay_rejects_caller_supplied_human_source(boundary_connection):
    payload = _payload(
        dispatch_replay_command(
            boundary_connection, "start_replay", {"recording_id": 1, "source": "human"}
        )
    )
    assert _error(payload) == "INVALID_FIELD"
    assert boundary_connection.started_run_ids == []


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
    peer = ReplayCommandPeer()
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
    peer = ReplayCommandPeer()
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
    peer = ReplayCommandPeer()
    peer.add_recording(1)
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
    peer = ReplayCommandPeer()
    payload = _payload(dispatch_replay_command(peer, command, {"run_id": bad}))
    assert _error(payload) == "INVALID_VALUE"
    assert peer.calls == []


@pytest.mark.parametrize("command", ["get_status", "start_capture", "list_human_recordings", "", "replay"])
def test_replay_unknown_command_is_rejected(command):
    peer = ReplayCommandPeer()
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


def test_replay_list_forwards_native_page_params(boundary_connection):
    boundary_connection.add_recording(3, description="Second enabled recording")
    result = dispatch_replay_command(boundary_connection, "list_recordings", {"page_size": 1})
    payload = _payload(result)
    assert [row["recording_id"] for row in payload["recordings"]] == [1]
    assert payload["has_more"] is True
    assert payload["next_after_recording_id"] == 1
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES
    assert ("replay.list_recordings", {"page_size": 1}) in boundary_connection.calls


# --------------------------------------------------------------------------------------
# Start admission, one-shot semantics and recovery
# --------------------------------------------------------------------------------------


def test_start_replay_returns_preparing_identity(boundary_connection):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": 1})
    )
    assert "_error" not in payload
    assert payload["state"] == "Preparing"
    assert payload["recording_id"] == 1
    assert payload["editor_instance_id"] == boundary_connection.editor_instance_id
    assert payload["run_id"] in boundary_connection.started_run_ids


def test_pre_dispatch_failure_reports_not_dispatched_without_admission(boundary_connection):
    boundary_connection.fail_next_start_presend = True
    payload = _payload(
        dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": 1})
    )
    assert _error(payload) == "REPLAY_START_NOT_DISPATCHED"
    assert payload["outcome"] == "not_dispatched"
    assert payload["recovery_required"] is False
    assert boundary_connection.started_run_ids == []


def test_uncertain_start_reports_unknown_outcome_and_recovers_same_run(boundary_connection):
    boundary_connection.drop_next_start_ack = True
    payload = _payload(
        dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": 1})
    )
    assert _error(payload) == "REPLAY_START_OUTCOME_UNKNOWN"
    assert payload["outcome"] == "unknown"
    assert payload["recovery_required"] is True
    assert len(boundary_connection.started_run_ids) == 1
    run_id = boundary_connection.started_run_ids[0]

    listing = _payload(dispatch_replay_command(boundary_connection, "list_recordings", {}))
    assert listing["active_ai_run"]["run_id"] == run_id

    status = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))
    assert "_error" not in status
    assert status["state"] == "Preparing"
    assert status["run_id"] == run_id
    assert boundary_connection.native_start_count == 1


def test_lost_ack_then_completed_history(boundary_connection):
    boundary_connection.drop_next_start_ack = True
    payload = _payload(
        dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": 1})
    )
    assert _error(payload) == "REPLAY_START_OUTCOME_UNKNOWN"
    run_id = boundary_connection.started_run_ids[0]

    boundary_connection.advance_run(
        run_id, state="Completed", finalized_at_utc="2026-10-05T00:10:00.000Z"
    )
    boundary_connection.active_run_id = None

    listing = _payload(dispatch_replay_command(boundary_connection, "list_recordings", {}))
    recovered = {summary["run_id"]: summary for summary in listing["recent_ai_runs"]}
    assert run_id in recovered
    assert recovered[run_id]["state"] == "Completed"
    assert recovered[run_id]["editor_instance_id"] == boundary_connection.editor_instance_id
    assert recovered[run_id]["started_at_utc"] and recovered[run_id]["finalized_at_utc"]


def test_denied_recording_start_is_permission_denied_without_admission(boundary_connection):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": 2})
    )
    assert _error(payload) == "PERMISSION_DENIED"
    assert boundary_connection.started_run_ids == []


def test_native_command_error_preserves_structured_details(boundary_connection):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "get_recording", {"recording_id": 999})
    )
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
    assert boundary_connection.started_run_ids == []


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


def test_finalize_is_total_for_unserializable_and_surrogate_inputs():
    from cortex_mcp.replay_boundary import _finalize

    fallback = _payload(_finalize({"data": object()}))
    assert _error(fallback) == "REPLAY_REPLY_FORMAT_ERROR"

    # Unpaired surrogate: ASCII escaping keeps the reply valid and encodable.
    escaped = _finalize({"value": "\ud800"})
    json.loads(escaped)
    assert _utf8_size(escaped) <= MAX_RESPONSE_BYTES

    # A formatting failure must still carry start one-shot machine context.
    context = {
        "_command": "replay.start_replay",
        "outcome": "unknown",
        "recovery_required": True,
    }
    with_context = _payload(_finalize({"data": object()}, overflow_fields=context))
    assert _error(with_context) == "REPLAY_REPLY_FORMAT_ERROR"
    assert with_context["_command"] == "replay.start_replay"
    assert with_context["outcome"] == "unknown"
    assert with_context["recovery_required"] is True

    # Overflow may never let preserved fields override the budget contract fields.
    from cortex_mcp.replay_boundary import _limit_exceeded_response

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


def test_strict_router_rejections_use_the_replay_budget(boundary_connection):
    from cortex_mcp.tools.routers import _invalid_invocation_shape, make_router, strict_router_tool

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
    from cortex_mcp.replay_boundary import _finalize

    envelope = _invalid_invocation_shape(
        "Malformed replay_cmd envelope: " + "y" * 40000, finalize=_finalize
    )
    envelope_payload = _payload(envelope)
    assert _error(envelope_payload) == "LIMIT_EXCEEDED"
    assert envelope_payload["response_bytes"] > MAX_RESPONSE_BYTES
    assert _utf8_size(envelope) <= MAX_RESPONSE_BYTES
    assert boundary_connection.calls == []


def test_every_replay_reply_path_stays_within_the_utf8_budget(boundary_connection):
    from cortex_mcp.replay_boundary import _finalize
    from cortex_mcp.tools.routers import _invalid_invocation_shape

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
    from cortex_mcp.tools.routers import make_router, strict_router_tool

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
    from cortex_mcp.tools.routers import make_router, strict_router_tool

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
    assert boundary_connection.started_run_ids == []


def test_client_never_invents_success_or_reissues_start(boundary_connection):
    boundary_connection.drop_next_start_ack = True
    payload = _payload(
        dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": 1})
    )
    assert _error(payload) == "REPLAY_START_OUTCOME_UNKNOWN"
    assert payload["recovery_required"] is True
    assert payload.get("state") != "Completed"
    assert boundary_connection.native_start_count == 1

    run_id = boundary_connection.started_run_ids[0]
    status = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))
    assert status["state"] == "Preparing"
    assert boundary_connection.native_start_count == 1


# --------------------------------------------------------------------------------------
# get_recording read contract
# --------------------------------------------------------------------------------------


def test_get_recording_exposes_immutable_initial_state_and_hashes_without_inputs(boundary_connection):
    boundary_connection.add_recording(
        3,
        name="Press only",
        coverage={
            "scope": "press_only",
            "pose_presses": 3,
            "ui_supported_presses": 1,
            "ui_unavailable_presses": 2,
            "ui_not_applicable_presses": 0,
        },
    )
    result = dispatch_replay_command(boundary_connection, "get_recording", {"recording_id": 3})
    payload = _payload(result)

    assert payload["recording_id"] == 3
    assert payload["initial_state_sha256"] == "a" * 64
    assert payload["inputs_sha256"] == "b" * 64
    assert payload["recording_snapshot_sha256"] == "c" * 64
    assert payload["initial_state"]["pawn_class_path"] == "/Script/Engine.DefaultPawn"
    assert set(payload["initial_state"]["pawn_transform"]) == {"location_cm", "rotation_deg", "scale"}
    assert "control_rotation_deg" in payload["initial_state"]

    coverage = payload["guard_coverage"]
    assert coverage["scope"] == "press_only"
    assert (
        coverage["ui_supported_presses"]
        + coverage["ui_unavailable_presses"]
        + coverage["ui_not_applicable_presses"]
        == coverage["pose_presses"]
    )
    for forbidden in ("events", "inputs", "input_rows", "event_rows", "selector_catalog", "path"):
        assert forbidden not in payload
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES


def test_get_recording_denies_disabled_recording(boundary_connection):
    payload = _payload(
        dispatch_replay_command(boundary_connection, "get_recording", {"recording_id": 2})
    )
    assert _error(payload) == "PERMISSION_DENIED"


# --------------------------------------------------------------------------------------
# get_run status contract: bounded waiting, terminal diagnostics, recovery
# --------------------------------------------------------------------------------------


def test_execution_error_state_is_a_successful_status_query(boundary_connection):
    run_id = _start(boundary_connection)
    boundary_connection.advance_run(
        run_id,
        state="Error",
        finalized_at_utc="2026-10-05T00:10:00.000Z",
        execution_error={
            "code": "REPLAY_POSE_GUARD_FAILED",
            "message": "pose drifted beyond tolerance",
            "details": {"sequence": 0, "deviation_cm": 0.6},
        },
    )
    result = dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id})
    payload = _payload(result)

    assert "_error" not in payload
    assert payload["state"] == "Error"
    assert payload["guard_scope"] == "press_only"
    assert payload["execution_error"]["code"] == "REPLAY_POSE_GUARD_FAILED"
    assert payload["execution_error"]["details"]["deviation_cm"] == 0.6
    assert payload["waiting"] is None
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES


def test_get_run_reports_bounded_waiting_and_progress(boundary_connection):
    run_id = _start(boundary_connection)
    boundary_connection.advance_run(
        run_id,
        state="Replaying",
        dispatched_events=3,
        authorized_wait_seconds=0.5,
        waiting={
            "sequence": 4,
            "reason": "target_disabled",
            "elapsed_seconds": 0.4,
            "committed_wait_seconds": 0.0,
            "remaining_event_seconds": 0.6,
            "remaining_run_seconds": 4.5,
        },
    )
    payload = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))

    assert payload["state"] == "Replaying"
    assert payload["dispatched_events"] == 3
    assert 0.0 <= payload["authorized_wait_seconds"] <= 5.0
    waiting = payload["waiting"]
    assert waiting["sequence"] == 4
    for key in ("elapsed_seconds", "committed_wait_seconds", "remaining_event_seconds", "remaining_run_seconds"):
        assert 0.0 <= waiting[key] <= 5.0

    later = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))
    assert later["waiting"] == waiting
    assert later["run_id"] == run_id


@pytest.mark.parametrize(
    "code",
    ["REPLAY_POSE_GUARD_FAILED", "REPLAY_UI_GUARD_FAILED", "REPLAY_UI_WAIT_TIMEOUT"],
)
def test_guard_failure_wait_then_error_terminal(boundary_connection, code):
    run_id = _start(boundary_connection)
    boundary_connection.advance_run(
        run_id,
        state="Replaying",
        waiting={
            "sequence": 0,
            "reason": "pointer_pending",
            "elapsed_seconds": 0.2,
            "committed_wait_seconds": 0.0,
            "remaining_event_seconds": 0.8,
            "remaining_run_seconds": 4.8,
        },
    )
    waiting = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))
    assert waiting["waiting"]["sequence"] == 0

    boundary_connection.advance_run(
        run_id,
        state="Error",
        finalized_at_utc="2026-10-05T00:10:00.000Z",
        waiting=None,
        execution_error={"code": code, "message": "guard evaluation failed", "details": {"sequence": 0}},
    )
    terminal = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))
    assert "_error" not in terminal
    assert terminal["state"] == "Error"
    assert terminal["execution_error"]["code"] == code
    assert terminal["waiting"] is None


def test_oversized_status_diagnostics_are_an_explicit_contract_error(boundary_connection):
    run_id = _start(boundary_connection)
    boundary_connection.advance_run(
        run_id,
        state="Error",
        finalized_at_utc="2026-10-05T00:10:00.000Z",
        execution_error={
            "code": "REPLAY_POSE_GUARD_FAILED",
            "message": "m" * 60000,
            "details": {"sequence": 0},
        },
    )
    result = dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id})
    payload = _payload(result)
    assert _error(payload) == "LIMIT_EXCEEDED"
    assert "execution_error" not in payload
    assert _utf8_size(result) <= MAX_RESPONSE_BYTES


def test_waiting_run_cancelled_by_committed_permission_revocation(boundary_connection):
    run_id = _start(boundary_connection)
    boundary_connection.advance_run(
        run_id,
        state="Replaying",
        waiting={
            "sequence": 1,
            "reason": "target_disabled",
            "elapsed_seconds": 0.1,
            "committed_wait_seconds": 0.0,
            "remaining_event_seconds": 0.9,
            "remaining_run_seconds": 4.9,
        },
    )
    waiting = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))
    assert waiting["state"] == "Replaying"
    assert waiting["waiting"]["sequence"] == 1

    # Committed native revocation cancels the run without the client issuing a cancel command.
    boundary_connection.revoke(1)

    status = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))
    assert status["state"] == "Cancelled"
    assert status["waiting"] is None
    assert status["recording_snapshot_sha256"] == "c" * 64
    assert status["initial_state_sha256"] == "a" * 64
    assert status["inputs_sha256"] == "b" * 64
    assert all(command != "replay.cancel_replay" for command, _ in boundary_connection.calls)

    denied = _payload(dispatch_replay_command(boundary_connection, "start_replay", {"recording_id": 1}))
    assert _error(denied) == "PERMISSION_DENIED"
    assert boundary_connection.native_start_count == 1


def test_explicit_cancel_returns_terminal_state(boundary_connection):
    run_id = _start(boundary_connection)
    boundary_connection.advance_run(
        run_id,
        state="Replaying",
        waiting={
            "sequence": 2,
            "reason": "pointer_pending",
            "elapsed_seconds": 0.3,
            "committed_wait_seconds": 0.0,
            "remaining_event_seconds": 0.7,
            "remaining_run_seconds": 4.7,
        },
    )
    cancel = _payload(dispatch_replay_command(boundary_connection, "cancel_replay", {"run_id": run_id}))
    assert cancel["state"] == "Cancelled"
    status = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))
    assert status["state"] == "Cancelled"
    assert status["waiting"] is None
    assert any(command == "replay.cancel_replay" for command, _ in boundary_connection.calls)


@pytest.mark.parametrize("mutation", ["restart", "replace_library", "delete"])
def test_terminal_status_survives_restart_library_replacement_and_deletion(boundary_connection, mutation):
    run_id = _start(boundary_connection)
    boundary_connection.advance_run(
        run_id, state="Completed", finalized_at_utc="2026-10-05T00:10:00.000Z"
    )
    boundary_connection.active_run_id = None

    if mutation == "restart":
        boundary_connection.restart()
    elif mutation == "replace_library":
        boundary_connection.replace_library([])
    else:
        boundary_connection.delete_recording(1)

    payload = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))
    assert "_error" not in payload
    assert payload["state"] == "Completed"
    assert payload["recording_snapshot_sha256"] == "c" * 64
    assert payload["initial_state_sha256"] == "a" * 64
    assert payload["inputs_sha256"] == "b" * 64


def test_later_polls_retain_admitted_run_and_digests(boundary_connection):
    run_id = _start(boundary_connection)
    first = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))
    boundary_connection.advance_run(run_id, state="Replaying", dispatched_events=1)
    second = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": run_id}))
    for key in (
        "run_id",
        "recording_id",
        "recording_snapshot_sha256",
        "initial_state_sha256",
        "inputs_sha256",
        "guard_scope",
    ):
        assert second[key] == first[key]


def test_human_run_is_invisible_to_status(boundary_connection):
    human = boundary_connection.add_terminal_run(origin="human", state="Cancelled")
    payload = _payload(dispatch_replay_command(boundary_connection, "get_run", {"run_id": human}))
    assert _error(payload) == "RUN_NOT_FOUND"


# --------------------------------------------------------------------------------------
# Live pages: revocation, budget and recovery entries
# --------------------------------------------------------------------------------------


def test_revocation_between_pages_hides_unvisited_recording():
    peer = ReplayCommandPeer()
    for recording_id in range(1, 6):
        peer.add_recording(recording_id)

    first = _payload(dispatch_replay_command(peer, "list_recordings", {"page_size": 2}))
    assert [row["recording_id"] for row in first["recordings"]] == [1, 2]
    assert first["has_more"] is True
    cursor = first["next_after_recording_id"]
    assert cursor == 2

    peer.revoke(3)
    second = _payload(
        dispatch_replay_command(peer, "list_recordings", {"after_recording_id": cursor, "page_size": 2})
    )
    assert [row["recording_id"] for row in second["recordings"]] == [4, 5]
    assert 3 not in [row["recording_id"] for row in second["recordings"]]


def test_enabling_below_cursor_requires_fresh_first_page():
    peer = ReplayCommandPeer()
    for recording_id in range(1, 7):
        peer.add_recording(recording_id, ai_enabled=(recording_id != 2))

    first = _payload(dispatch_replay_command(peer, "list_recordings", {"page_size": 2}))
    assert [row["recording_id"] for row in first["recordings"]] == [1, 3]
    cursor = first["next_after_recording_id"]
    assert cursor == 3

    peer.recordings[2]["ai_enabled"] = True
    with_cursor = _payload(
        dispatch_replay_command(peer, "list_recordings", {"after_recording_id": cursor, "page_size": 5})
    )
    assert 2 not in [row["recording_id"] for row in with_cursor["recordings"]]

    fresh = _payload(dispatch_replay_command(peer, "list_recordings", {"page_size": 5}))
    assert 2 in [row["recording_id"] for row in fresh["recordings"]]


def test_multi_page_library_preserves_rows_and_recovery_within_budget():
    peer = ReplayCommandPeer()
    worst_name = _worst_case_text(128)
    worst_description = _worst_case_text(1024)
    for recording_id in range(1, 31):
        peer.add_recording(
            recording_id,
            ai_enabled=(recording_id != 5),
            name=worst_name,
            description=worst_description,
        )

    for index in range(100):
        peer.add_terminal_run(
            recording_id=1,
            state="Cancelled",
            started_at_utc="2026-10-05T00:00:00.000Z",
            finalized_at_utc=f"2026-10-05T01:00:{index:02d}.000Z",
        )
    human_run = peer.add_terminal_run(
        recording_id=1, origin="human", state="Cancelled", finalized_at_utc="2026-10-05T02:00:00.000Z"
    )

    active_run_id = _start(peer)
    eligible = {recording_id for recording_id in range(1, 31) if recording_id != 5}

    visited: set[int] = set()
    revoked: set[int] = set()
    after = None
    pages = 0
    aggregate_row_bytes = 0

    while True:
        params: dict = {"page_size": 100}
        if after is not None:
            params["after_recording_id"] = after
        result = dispatch_replay_command(peer, "list_recordings", params)
        payload = _payload(result)

        assert _utf8_size(result) <= MAX_RESPONSE_BYTES
        assert "_truncated" not in payload
        assert payload["recovery_window_seconds"] == 86400
        assert payload["recovery_max_terminal_runs"] == 100
        assert len(payload["recent_ai_runs"]) == 100
        assert human_run not in {summary["run_id"] for summary in payload["recent_ai_runs"]}
        assert payload["active_ai_run"]["run_id"] == active_run_id

        rows = payload["recordings"]
        assert rows
        previous = after or 0
        for row in rows:
            recording_id = row["recording_id"]
            assert recording_id > previous
            assert recording_id in eligible and recording_id not in revoked
            assert recording_id not in visited
            visited.add(recording_id)
            assert row["map_asset_path"] == "/Game/Maps/TestMap"
            assert row["name"] == worst_name
            assert row["description"] == worst_description
            assert row["initial_state_sha256"] == "a" * 64
            assert row["inputs_sha256"] == "b" * 64
            assert row["guard_coverage"]["scope"] == "press_only"
            assert "initial_state" not in row
            previous = recording_id
            aggregate_row_bytes += _utf8_size(json.dumps(row, ensure_ascii=False, separators=(",", ":")))

        if payload["has_more"]:
            assert payload["next_after_recording_id"] == rows[-1]["recording_id"]
            after = payload["next_after_recording_id"]
        else:
            assert payload["next_after_recording_id"] is None
            break

        if pages == 0:
            candidates = sorted(recording_id for recording_id in eligible if recording_id > after)
            revoked.add(candidates[-1])
            peer.revoke(candidates[-1])
        pages += 1

    assert pages > 1
    assert aggregate_row_bytes > MAX_RESPONSE_BYTES
    assert visited == eligible - revoked


def test_oversized_native_reply_is_an_explicit_contract_error():
    peer = ReplayCommandPeer()
    # A single row far above the 39,000-byte page budget simulates a native contract violation.
    peer.add_recording(1, description="x" * 60000)
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
    peer = ReplayCommandPeer()
    peer.add_recording(1)
    router = _core_router(peer)
    payload = _payload(
        router(command, {key: [{"command": "replay.start_replay", "params": {"recording_id": 1}}]})
    )
    assert _error(payload) == "INVALID_OPERATION"
    assert peer.started_run_ids == []
    assert peer.calls == []


def test_core_batch_preserves_unrelated_batches():
    peer = ReplayCommandPeer()
    peer.add_recording(1)
    router = _core_router(peer)

    reads = _payload(
        router(
            "batch_query",
            {"commands": [{"command": "replay.get_recording", "params": {"recording_id": 1}}]},
        )
    )
    assert "_error" not in reads
    assert any(command == "batch" for command, _ in peer.calls)

    unrelated_peer = ReplayCommandPeer()
    unrelated_router = _core_router(unrelated_peer)
    unrelated = _payload(
        unrelated_router("batch_query", {"commands": [{"command": "core.get_status", "params": {}}]})
    )
    assert "_error" not in unrelated
    assert any(command == "batch" for command, _ in unrelated_peer.calls)


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
