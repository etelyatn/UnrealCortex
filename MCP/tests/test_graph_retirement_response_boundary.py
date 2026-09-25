"""Tests for bounded graph retirement previews and mutation outcomes."""

from __future__ import annotations

import json
from unittest.mock import MagicMock

import pytest

from cortex_mcp.response import MAX_RESPONSE_CHARS
from cortex_mcp.tcp_client import UECommandError


def _boundary_function(name):
    from importlib import import_module

    module = import_module("cortex_mcp.graph_patch_boundary")
    function = getattr(module, name, None)
    assert callable(function), f"required boundary function {name} is not available"
    return function


def dispatch_graph_apply_patch(*args, **kwargs):
    return _boundary_function("dispatch_graph_apply_patch")(*args, **kwargs)


def complete_approval_migration_op(*args, **kwargs):
    return _boundary_function("complete_approval_migration_op")(*args, **kwargs)


def _guids(count: int) -> list[str]:
    return [f"00000000-0000-0000-0000-{i:012d}" for i in range(count)]


def retire_request(*, dry_run: bool, approved: list[str] | None = None) -> dict:
    migration = {
        "op": "retire_entries",
        "source": {
            "graph_ref": {"graph_guid": "11111111-1111-1111-1111-111111111111"},
            "entry_node_guids": ["22222222-2222-2222-2222-222222222222"],
        },
    }
    if approved is not None:
        migration["approved_node_guids"] = approved
    return {
        "asset_path": "/Game/Temp/WBP_Retire.WBP_Retire",
        "patch_id": "33333333-3333-3333-3333-333333333333",
        "expected_fingerprint": {"graph_authoring_version": 1, "graph_authoring_hash": "hash"},
        "migration": migration,
        "nodes": [],
        "connections": [],
        "pin_updates": [],
        "dry_run": dry_run,
        "compile": False,
        "save": False,
        "allow_noop": False,
    }


def _preview(
    count: int,
    *,
    token: str = "retire-token",
    complete: bool = True,
    approved_guids: list[str] | None = None,
) -> dict:
    return {
        "patch_id": "33333333-3333-3333-3333-333333333333",
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
        "fingerprint_before": "fingerprint-before",
        "fingerprint_after": "fingerprint-before",
        "dirty_before": False,
        "dirty_after": False,
        "locators": [],
        "validation_hash": token,
        "complete": complete,
        "removable": _guids(count),
        "approved_guids": approved_guids or [],
        "diagnostics": [],
    }


def _payload(result: str) -> dict:
    return json.loads(result)


def test_complete_retirement_preview_returns_every_removable_guid_within_budget():
    connection = MagicMock()
    preview = _preview(100)
    connection.send_command.return_value = {"success": True, "data": preview}

    payload = _payload(dispatch_graph_apply_patch(
        connection, retire_request(dry_run=True), tool_name="graph_cmd",
    ))

    assert payload["complete"] is True
    assert payload["removable"] == _guids(100)
    assert payload["validation_hash"] == "retire-token"
    assert len(json.dumps(payload, indent=2)) <= MAX_RESPONSE_CHARS
    connection.send_command.assert_called_once()
    connection.send_command_once.assert_not_called()


def test_oversized_retirement_preview_refuses_before_mutation():
    connection = MagicMock()
    count = 3000
    connection.send_command.return_value = {"success": True, "data": _preview(count)}

    payload = _payload(dispatch_graph_apply_patch(
        connection, retire_request(dry_run=True), tool_name="graph_cmd",
    ))

    assert payload["success"] is False
    assert payload["_error"] == "LIMIT_EXCEEDED"
    assert payload["approval_complete"] is False
    assert payload["complete"] is True
    assert payload["removable_count"] == count
    assert len(json.dumps(payload, indent=2)) <= MAX_RESPONSE_CHARS
    connection.send_command_once.assert_not_called()


def test_approved_retirement_apply_uses_one_bounded_preview_and_one_single_send():
    connection = MagicMock()
    approved = _guids(2)
    request = retire_request(dry_run=False, approved=approved)
    request["expected_validation_hash"] = "retire-token"
    connection.send_command.return_value = {
        "success": True,
        "data": _preview(2, approved_guids=approved),
    }
    connection.send_command_once.return_value = {
        "success": True,
        "data": {"patch_id": request["patch_id"], "changed": True},
    }

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["patch_id"] == request["patch_id"]
    connection.send_command.assert_called_once()
    preview_request = connection.send_command.call_args.args[1]
    assert preview_request["dry_run"] is True
    assert preview_request["save"] is False
    assert "expected_validation_hash" not in preview_request
    connection.send_command_once.assert_called_once_with("graph.apply_patch", request)


@pytest.mark.parametrize(
    ("token", "native_approved"),
    [("changed-token", _guids(1)), ("retire-token", _guids(2))],
)
def test_retirement_apply_refuses_changed_token_or_approved_set(token, native_approved):
    connection = MagicMock()
    approved = _guids(1)
    request = retire_request(dry_run=False, approved=approved)
    request["expected_validation_hash"] = "retire-token"
    connection.send_command.return_value = {
        "success": True,
        "data": _preview(2, token=token, approved_guids=native_approved),
    }

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["_error"] == "STALE_PRECONDITION"
    connection.send_command.assert_called_once()
    connection.send_command_once.assert_not_called()


@pytest.mark.parametrize("field", ["limit", "cursor", "offset", "page"])
def test_retirement_pagination_is_rejected_before_dispatch(field):
    connection = MagicMock()
    request = retire_request(dry_run=True)
    request[field] = 1

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["success"] is False
    assert payload["_error"] == "INVALID_FIELD"
    connection.send_command.assert_not_called()
    connection.send_command_once.assert_not_called()


def test_retirement_apply_connection_loss_reports_unknown_outcome_and_reconciliation():
    connection = MagicMock()
    approved = _guids(1)
    request = retire_request(dry_run=False, approved=approved)
    request["expected_validation_hash"] = "retire-token"
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    connection.send_command_once.side_effect = ConnectionError("response lost")

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["_error"] == "UNKNOWN_OUTCOME"
    assert payload["reconciliation_required"] is True
    assert "read back" in payload["_reconciliation_guidance"].lower()
    connection.send_command_once.assert_called_once_with("graph.apply_patch", request)


def test_oversized_retirement_apply_success_preserves_phase_fields():
    connection = MagicMock()
    approved = _guids(1)
    request = retire_request(dry_run=False, approved=approved)
    request["expected_validation_hash"] = "retire-token"
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    result = _preview(1, approved_guids=approved)
    result.update({
        "success": True,
        "changed": True,
        "dry_run": False,
        "apply_status": "applied",
        "compile_status": "compiled",
        "readback_status": "verified",
        "rollback_status": "not_needed",
        "save_status": "saved",
        "post_save_status": "verified",
        "target_compile_count": 1,
        "saved": True,
        "diagnostics": ["x" * MAX_RESPONSE_CHARS],
    })
    connection.send_command_once.return_value = {"success": True, "data": result}

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["_truncated"] is True
    assert payload["apply_status"] == "applied"
    assert payload["compile_status"] == "compiled"
    assert payload["readback_status"] == "verified"
    assert payload["rollback_status"] == "not_needed"
    assert payload["save_status"] == "saved"
    assert payload["post_save_status"] == "verified"
    assert payload["saved"] is True
    assert len(json.dumps(payload, indent=2)) <= MAX_RESPONSE_CHARS


def test_retirement_native_error_preserves_error_envelope_and_details():
    connection = MagicMock()
    approved = _guids(1)
    request = retire_request(dry_run=False, approved=approved)
    request["expected_validation_hash"] = "retire-token"
    connection.send_command.return_value = {
        "success": True, "data": _preview(1, approved_guids=approved),
    }
    connection.send_command_once.side_effect = UECommandError(
        "graph.apply_patch", "RETIRE_FAILED", "retirement failed", {"rollback_status": "restored"},
    )

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["_error"] == "RETIRE_FAILED"
    assert payload["_message"] == "retirement failed"
    assert payload["_command"] == "graph.apply_patch"
    assert payload["rollback_status"] == "restored"


def test_complete_approval_predicate_recognizes_only_supported_migrations():
    predicate = complete_approval_migration_op
    assert predicate(retire_request(dry_run=True)) == "retire_entries"
    assert predicate({"migration": {"op": "prune_island"}}) == "prune_island"
    assert predicate({"migration": {"op": "add_node"}}) is None
    assert predicate({}) is None


@pytest.mark.parametrize("op", [[], {}])
def test_complete_approval_predicate_rejects_non_string_migration_op(op):
    predicate = complete_approval_migration_op
    assert predicate({"migration": {"op": op}}) is None
