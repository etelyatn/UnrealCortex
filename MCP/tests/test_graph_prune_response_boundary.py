"""Tests for bounded graph prune previews and mutation outcomes."""

from __future__ import annotations

import json
from unittest.mock import MagicMock

import pytest

from cortex_mcp.response import MAX_RESPONSE_CHARS
from cortex_mcp.tcp_client import UECommandError


def _boundary_function(name):
    from importlib import import_module

    try:
        module = import_module("cortex_mcp.graph_patch_boundary")
    except ModuleNotFoundError:
        module = None
    function = getattr(module, name, None) if module else None
    assert callable(function), f"required boundary function {name} is not available"
    return function


def dispatch_graph_apply_patch(*args, **kwargs):
    return _boundary_function("dispatch_graph_apply_patch")(*args, **kwargs)


def is_prune_patch(*args, **kwargs):
    return _boundary_function("is_prune_patch")(*args, **kwargs)


def reject_apply_patch_pagination(*args, **kwargs):
    return _boundary_function("reject_apply_patch_pagination")(*args, **kwargs)

ASSET = "/Game/Test/BP_Test.BP_Test"
PATCH_ID = "patch-1"
SOURCE = {"node_guid": "source-node", "pin_name": "Then"}
FINGERPRINT = "fingerprint-1"


def _guids(count: int) -> list[str]:
    return [f"00000000-0000-0000-0000-{i:012d}" for i in range(count)]


def _preview(
    count: int,
    *,
    token: str = "token-123",
    complete: bool = True,
    approved_guids: list[str] | None = None,
) -> dict:
    return {
        "patch_id": PATCH_ID,
        "changed": False,
        "dry_run": True,
        "apply_status": "not_started",
        "compile_status": "not_requested",
        "readback_status": "not_requested",
        "rollback_status": "not_needed",
        "save_status": "not_requested",
        "post_save_status": "not_requested",
        "target_compile_count": 0,
        "recovery_compile_count": 0,
        "saved": False,
        "blocked": False,
        "replayed_with_absent_source": False,
        "fingerprint_before": FINGERPRINT,
        "fingerprint_after": FINGERPRINT,
        "dirty_before": False,
        "dirty_after": False,
        "locators": [],
        "validation_hash": token,
        "complete": complete,
        "scan_limit": 4096,
        "scanned_nodes": 4096,
        "scanned_links": 8192,
        "removable": _guids(count),
        "approved_guids": approved_guids or [],
        "diagnostics": [],
    }


def _request(**changes) -> dict:
    request = {
        "asset_path": ASSET,
        "patch_id": PATCH_ID,
        "expected_fingerprint": FINGERPRINT,
        "migration": {
            "op": "prune_island",
            "source": SOURCE,
            "approved_node_guids": [],
        },
        "dry_run": True,
        "compile": True,
        "save": False,
    }
    request.update(changes)
    return request


def _payload(result: str) -> dict:
    return json.loads(result)


def test_is_prune_patch_requires_exact_migration_operation():
    assert is_prune_patch(_request()) is True
    assert is_prune_patch({"migration": {"op": "other"}}) is False
    assert is_prune_patch({"migration": "prune_island"}) is False
    assert is_prune_patch({}) is False


def test_reject_apply_patch_pagination_only_rejects_prune_pagination():
    assert reject_apply_patch_pagination(_request(limit=10))
    assert reject_apply_patch_pagination(_request(cursor="cursor"))
    assert reject_apply_patch_pagination(_request()) is None
    assert reject_apply_patch_pagination({"limit": 10}) is None


def test_prune_preview_at_supported_boundary_returns_every_guid_without_truncation():
    connection = MagicMock()
    candidate = _preview(100)
    connection.send_command.return_value = {"success": True, "data": candidate}
    request = _request()

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["validation_hash"] == "token-123"
    assert payload["removable"] == _guids(100)
    assert "_truncated" not in payload
    assert len(json.dumps(payload, indent=2)) <= MAX_RESPONSE_CHARS
    connection.send_command.assert_called_once_with("graph.apply_patch", request)


def test_prune_preview_over_boundary_refuses_with_limit_exceeded():
    connection = MagicMock()
    count = 3000
    connection.send_command.return_value = {"success": True, "data": _preview(count)}

    payload = _payload(dispatch_graph_apply_patch(connection, _request(), tool_name="graph_cmd"))

    assert payload["success"] is False
    assert payload["_error"] == "LIMIT_EXCEEDED"
    assert payload["approval_complete"] is False
    assert payload["response_size_chars"] > MAX_RESPONSE_CHARS
    assert payload["max_response_chars"] == MAX_RESPONSE_CHARS
    assert payload["removable_count"] == count
    assert payload["approved_count"] == 0
    assert payload["complete"] is True
    assert "_truncated" not in payload
    assert len(json.dumps(payload, indent=2)) <= MAX_RESPONSE_CHARS


def test_prune_partition_preview_with_omitted_dry_run_uses_native_preview_once():
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": _preview(2)}
    request = _request()
    request.pop("dry_run")

    dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd")

    connection.send_command.assert_called_once_with("graph.apply_patch", request)


def test_apply_preflight_transforms_request_once_and_applies_original_with_caller_token():
    connection = MagicMock()
    approved = _guids(2)
    request = _request(
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
        dry_run=False,
        compile=True,
        save=True,
        expected_validation_hash="token-123",
    )
    original = json.loads(json.dumps(request))
    connection.send_command.return_value = {
        "success": True,
        "data": _preview(2, token="token-123", approved_guids=approved),
    }
    connection.send_command_once.return_value = {"success": True, "data": {"patch_id": PATCH_ID}}

    expected_preview = {
        "asset_path": ASSET,
        "patch_id": PATCH_ID,
        "expected_fingerprint": FINGERPRINT,
        "migration": {"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
        "dry_run": True,
        "compile": True,
        "save": False,
    }
    dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd")

    connection.send_command.assert_called_once_with("graph.apply_patch", expected_preview)
    connection.send_command_once.assert_called_once_with("graph.apply_patch", request)
    assert request == original

def test_apply_token_mismatch_stops_after_single_preview():
    connection = MagicMock()
    approved = _guids(1)
    request = _request(
        dry_run=False,
        expected_validation_hash="old-token",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    connection.send_command.return_value = {
        "success": True,
        "data": _preview(1, token="new-token", approved_guids=approved),
    }

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["success"] is False
    assert payload["_error"] == "STALE_PRECONDITION"
    connection.send_command.assert_called_once()
    connection.send_command_once.assert_not_called()
@pytest.mark.parametrize("token", [None, ""])
def test_missing_or_empty_apply_token_fails_closed(token):
    connection = MagicMock()
    approved = _guids(1)
    request = _request(
        dry_run=False,
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    if token is not None:
        request["expected_validation_hash"] = token
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    connection.send_command_once.return_value = {"success": True, "data": {"patch_id": PATCH_ID}}

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload.get("success") is False
    assert payload["_error"] == "STALE_PRECONDITION"
    connection.send_command_once.assert_not_called()


def test_apply_does_not_infer_approved_set_from_unapproved_preview():
    connection = MagicMock()
    request = _request(
        dry_run=False,
        expected_validation_hash="token-123",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": []},
    )
    connection.send_command.return_value = {
        "success": True, "data": _preview(2, approved_guids=_guids(2)),
    }
    connection.send_command_once.return_value = {"success": True, "data": {"patch_id": PATCH_ID}}

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload.get("success") is False
    assert payload["_error"] == "STALE_PRECONDITION"
    connection.send_command_once.assert_not_called()


def test_apply_refuses_prospective_response_that_exceeds_budget():
    connection = MagicMock()
    approved = _guids(400)
    request = _request(
        patch_id="p" * 5000,
        dry_run=False,
        expected_validation_hash="token-123",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    connection.send_command.return_value = {
        "success": True,
        "data": _preview(400, token="token-123", approved_guids=approved),
    }

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["_error"] == "LIMIT_EXCEEDED"
    assert payload["approval_complete"] is False
    assert payload["prospective_apply_size_chars"] > MAX_RESPONSE_CHARS
    assert payload["max_response_chars"] == MAX_RESPONSE_CHARS
    assert payload["removable_count"] == 400
    assert payload["approved_count"] == 400
    assert len(json.dumps(payload, indent=2)) <= MAX_RESPONSE_CHARS
    connection.send_command.assert_called_once()
    connection.send_command_once.assert_not_called()


def test_approved_apply_uses_one_exact_preview_and_caller_token():
    connection = MagicMock()
    approved = list(reversed(_guids(3)))
    request = _request(
        dry_run=False,
        expected_validation_hash="approval-token",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    native_approved = sorted(approved)
    connection.send_command.return_value = {
        "success": True,
        "data": _preview(3, token="approval-token", approved_guids=native_approved),
    }
    connection.send_command_once.return_value = {"success": True, "data": {"patch_id": PATCH_ID}}

    result = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert result["patch_id"] == PATCH_ID
    connection.send_command.assert_called_once()
    preview_request = connection.send_command.call_args.args[1]
    assert preview_request["migration"]["approved_node_guids"] == approved
    assert "expected_validation_hash" not in preview_request
    connection.send_command_once.assert_called_once_with("graph.apply_patch", request)


def test_non_prune_request_makes_one_native_call():
    connection = MagicMock()
    request = {"asset_path": ASSET, "dry_run": False}
    connection.send_command.return_value = {"success": True, "data": {"changed": True}}

    dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd")

    connection.send_command.assert_called_once_with("graph.apply_patch", request)
    connection.send_command_once.assert_not_called()


def test_apply_preserves_authoritative_native_outcome_fields():
    connection = MagicMock()
    approved = _guids(1)
    request = _request(
        dry_run=False,
        expected_validation_hash="token-123",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    data = _preview(1, approved_guids=approved)
    data.update({
        "changed": True, "dry_run": False, "apply_status": "applied",
        "compile_status": "compiled", "readback_status": "verified",
        "rollback_status": "not_needed", "save_status": "saved",
        "post_save_status": "verified", "target_compile_count": 1,
        "recovery_compile_count": 0, "saved": True, "blocked": False,
        "replayed_with_absent_source": False, "fingerprint_before": "before",
        "fingerprint_after": "after", "dirty_before": False, "dirty_after": False,
        "locators": [{"node_guid": "n1"}],
    })
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    connection.send_command_once.return_value = {"success": True, "data": data}

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    for key, value in data.items():
        assert payload[key] == value


def test_native_apply_error_preserves_all_present_outcome_details():
    connection = MagicMock()
    approved = _guids(1)
    request = _request(
        dry_run=False,
        expected_validation_hash="token-123",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    details = {"apply_status": "failed", "rollback_status": "restored", "saved": False,
               "diagnostics": [{"code": "failure"}], "target_compile_count": 0}
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    connection.send_command_once.side_effect = UECommandError(
        "graph.apply_patch", "APPLY_FAILED", "native failure", details,
    )

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["success"] is False
    assert payload["_error"] == "APPLY_FAILED"
    assert payload["_message"] == "native failure"
    assert payload["_command"] == "graph.apply_patch"
    for key, value in details.items():
        assert payload[key] == value

def test_apply_transport_failure_reports_unknown_outcome_without_retry():
    connection = MagicMock()
    approved = _guids(1)
    request = _request(
        dry_run=False,
        expected_validation_hash="token-123",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    connection.send_command_once.side_effect = ConnectionError("response lost")

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["success"] is False
    assert payload["_error"] == "UNKNOWN_OUTCOME"
    assert payload["patch_id"] == PATCH_ID
    assert payload["asset_path"] == ASSET
    assert payload["reconciliation_required"] is True
    assert "reconcil" in payload["_message"].lower()
    connection.send_command_once.assert_called_once()


def test_oversized_native_error_is_bounded_with_explicit_truncation():
    connection = MagicMock()
    approved = _guids(1)
    request = _request(
        dry_run=False,
        expected_validation_hash="token-123",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    details = {"apply_status": "failed", "rollback_status": "restored", "saved": False,
               "diagnostics": [{"text": "d" * 30000} for _ in range(3)]}
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    connection.send_command_once.side_effect = UECommandError(
        "graph.apply_patch", "APPROVAL_MISMATCH", "mismatch " + "m" * 50000, details,
    )

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["success"] is False
    assert payload["_error"] == "APPROVAL_MISMATCH"
    assert payload["_command"] == "graph.apply_patch"
    assert payload["_message"].startswith("mismatch ")
    assert payload["apply_status"] == "failed"
    assert payload["rollback_status"] == "restored"
    assert payload["saved"] is False
    assert payload["_truncated"] is True
    assert payload["original_response_size_chars"] > MAX_RESPONSE_CHARS
    assert payload["_message_truncated"] is True
    assert payload["original_message_chars"] > MAX_RESPONSE_CHARS
    assert payload["_diagnostics_omitted"] == 3
    assert payload["reconciliation_required"] is True
    assert payload["_reconciliation_guidance"]
    assert len(json.dumps(payload, indent=2)) <= MAX_RESPONSE_CHARS


def test_oversized_post_apply_future_field_is_bounded_and_outcomes_survive():
    connection = MagicMock()
    approved = _guids(1)
    request = _request(
        dry_run=False,
        expected_validation_hash="token-123",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    data = _preview(1, approved_guids=approved)
    data.update({
        "changed": True, "dry_run": False, "apply_status": "applied",
        "compile_status": "compiled", "readback_status": "verified",
        "rollback_status": "not_needed", "save_status": "saved",
        "post_save_status": "verified", "target_compile_count": 1,
        "recovery_compile_count": 0, "saved": True, "blocked": False,
        "replayed_with_absent_source": False, "dirty_before": False,
        "dirty_after": False, "future_bulk_field": "x" * 50000,
        "diagnostics": [{"text": "d" * 50000}],
    })
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    connection.send_command_once.return_value = {"success": True, "data": data}

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    for key in ("patch_id", "changed", "dry_run", "apply_status", "compile_status",
                "readback_status", "rollback_status", "save_status", "post_save_status",
                "target_compile_count", "recovery_compile_count", "saved", "blocked",
                "replayed_with_absent_source", "dirty_before", "dirty_after"):
        assert payload[key] == data[key]
    assert payload["_truncated"] is True
    assert payload["_omitted_fields"]
    assert payload["reconciliation_required"] is True
    assert len(json.dumps(payload, indent=2)) <= MAX_RESPONSE_CHARS

def test_oversized_nested_fingerprints_are_omitted_from_success_envelope():
    connection = MagicMock()
    approved = _guids(1)
    request = _request(
        dry_run=False,
        expected_validation_hash="token-123",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    data = _preview(1, approved_guids=approved)
    nested = {"fingerprint_data": {"items": ["x" * 2000 for _ in range(30)]}}
    data["fingerprint_before"] = nested
    data["fingerprint_after"] = nested
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    connection.send_command_once.return_value = {"success": True, "data": data}

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert len(json.dumps(payload, indent=2)) <= MAX_RESPONSE_CHARS
    assert payload["_truncated"] is True
    assert payload["reconciliation_required"] is True
    assert payload["_reconciliation_guidance"]
    assert "fingerprint_before" in payload["_omitted_fields"]
    assert "fingerprint_after" in payload["_omitted_fields"]


def test_oversized_nested_fingerprints_are_omitted_from_error_envelope():
    connection = MagicMock()
    approved = _guids(1)
    request = _request(
        dry_run=False,
        expected_validation_hash="token-123",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    nested = {"fingerprint_data": {"items": ["x" * 2000 for _ in range(30)]}}
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    connection.send_command_once.side_effect = UECommandError(
        "graph.apply_patch",
        "APPLY_FAILED",
        "failed",
        {"apply_status": "failed", "fingerprint_before": nested, "fingerprint_after": nested},
    )

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["success"] is False
    assert payload["_error"] == "APPLY_FAILED"
    assert len(json.dumps(payload, indent=2)) <= MAX_RESPONSE_CHARS
    assert payload["_truncated"] is True
    assert payload["reconciliation_required"] is True
    assert payload["_reconciliation_guidance"]
    assert "fingerprint_before" in payload["_omitted_fields"]
    assert "fingerprint_after" in payload["_omitted_fields"]


def test_oversized_unknown_outcome_is_always_bounded():
    connection = MagicMock()
    approved = _guids(1)
    request = _request(
        patch_id=PATCH_ID,
        dry_run=False,
        expected_validation_hash="token-123",
        migration={"op": "prune_island", "source": SOURCE, "approved_node_guids": approved},
    )
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    connection.send_command_once.side_effect = ConnectionError("lost " + "x" * 50000)

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["success"] is False
    assert payload["_error"] == "UNKNOWN_OUTCOME"
    assert payload["reconciliation_required"] is True
    assert payload["_truncated"] is True
    assert len(json.dumps(payload, indent=2)) <= MAX_RESPONSE_CHARS


def test_non_boolean_dry_run_is_forwarded_once_without_coercion():
    connection = MagicMock()
    request = _request(dry_run=0)
    connection.send_command.return_value = {"success": False, "error": {"code": "INVALID_ARGUMENT"}}

    dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd")

    connection.send_command.assert_called_once_with("graph.apply_patch", request)


class TestRejectPagination:
    @pytest.mark.parametrize("field", ["limit", "cursor", "offset", "page"])
    def test_rejects_pagination_options_for_prune(self, field):
        assert reject_apply_patch_pagination(_request(**{field: 1})) is not None
