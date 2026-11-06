"""Tests for bounded graph retirement previews and mutation outcomes."""

from __future__ import annotations

import json
from unittest.mock import MagicMock

import pytest

from cortex_mcp.response import MAX_RESPONSE_CHARS, compact_json
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


def retire_request(
    *, dry_run: bool, approved: list[str] | None = None, additional: list[str] | None = None,
) -> dict:
    migration = {
        "op": "retire_entries",
        "source": {
            "graph_ref": {"graph_guid": "11111111-1111-1111-1111-111111111111"},
            "entry_node_guids": ["22222222-2222-2222-2222-222222222222"],
        },
    }
    if additional is not None:
        migration["source"]["additional_node_guids"] = additional
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
    assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS
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
    assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS
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
    assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS


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


def test_retirement_incomplete_native_preview_refuses_independently_of_response_size():
    """A small response is still refused when the native scan is incomplete.

    The response budget and native completeness are separate refusals: a preview the budget could
    carry is never accepted while the native inventory says it is not the complete scan.
    """
    connection = MagicMock()
    preview = _preview(1, complete=False)
    connection.send_command.return_value = {"success": True, "data": preview}
    assert len(compact_json(preview)) <= MAX_RESPONSE_CHARS

    payload = _payload(dispatch_graph_apply_patch(
        connection, retire_request(dry_run=True), tool_name="graph_cmd",
    ))

    assert payload["success"] is False
    assert payload["_error"] == "LIMIT_EXCEEDED"
    assert payload["approval_complete"] is False
    assert payload["complete"] is False
    assert "incomplete" in payload["_message"]
    assert "exceeds the MCP response budget" not in payload["_message"]
    connection.send_command_once.assert_not_called()


def test_retirement_oversized_preview_never_publishes_a_partial_inventory():
    """The 40k refusal is independent of native completeness and drops no approval-critical array.

    The native preview claims `complete: true` and carries the exact identity arrays, but the
    response does not fit the bridge budget: the caller must receive the refusal alone, never a
    truncated prefix, so a `complete: true` prefix can never be mistaken for the approved set.
    """
    connection = MagicMock()
    count = 3000
    preview = _preview(count)
    preview["selected_entries"] = _guids(count)
    preview["shared"] = _guids(count)
    preview["blocked_nodes"] = _guids(count)
    preview["external_edges"] = _guids(count)
    preview["orphan_component_guids"] = _guids(count)
    preview["orphan_component_nodes"] = _guids(count)
    connection.send_command.return_value = {"success": True, "data": preview}

    payload = _payload(dispatch_graph_apply_patch(
        connection, retire_request(dry_run=True), tool_name="graph_cmd",
    ))

    assert payload["success"] is False
    assert payload["_error"] == "LIMIT_EXCEEDED"
    assert payload["approval_complete"] is False
    assert payload["complete"] is True          # the native claim is reported, never trusted
    assert payload["response_size_chars"] > MAX_RESPONSE_CHARS
    for key in ("removable", "selected_entries", "shared", "blocked_nodes", "external_edges",
                "orphan_component_guids", "orphan_component_nodes"):
        assert key not in payload, f"the refusal published a partial {key}"
    assert "_omitted_fields" not in payload
    assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS
    connection.send_command_once.assert_not_called()


def test_retirement_discovery_hash_cannot_authorize_apply():
    """The discovery preview hash is not the reviewed hash and can never authorize a removal."""
    connection = MagicMock()
    approved = _guids(2)
    request = retire_request(dry_run=False, approved=approved)
    request["expected_validation_hash"] = "discovery-token"
    connection.send_command.return_value = {
        "success": True,
        "data": _preview(2, token="reviewed-token", approved_guids=approved),
    }

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["_error"] == "STALE_PRECONDITION"
    assert "validation hash changed" in payload["_message"]
    connection.send_command.assert_called_once()
    connection.send_command_once.assert_not_called()


@pytest.mark.parametrize("field", ["additional_node_guids", "orphan_component_node_guids"])
def test_retirement_changed_reviewed_node_list_refuses_with_the_reviewed_token(field):
    """A changed additional GUID list is reviewed intent: the earlier token cannot authorize it.

    The bridge recomputes the preview over the caller's own source fields, so the requested GUIDs the
    native validation hash binds are forwarded verbatim and a list changed after the review produces
    a different hash instead of a removal.
    """
    connection = MagicMock()
    reviewed = ["00000000-0000-0000-0000-000000000101", "00000000-0000-0000-0000-000000000102"]
    changed = ["00000000-0000-0000-0000-000000000101", "00000000-0000-0000-0000-000000000103"]
    approved = _guids(2)
    request = retire_request(dry_run=False, approved=approved, additional=changed)
    request["migration"]["source"].pop("additional_node_guids")
    request["migration"]["source"][field] = changed
    request["expected_validation_hash"] = "reviewed-token"
    connection.send_command.return_value = {
        "success": True,
        "data": _preview(2, token="changed-source-token", approved_guids=approved),
    }

    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))

    assert payload["_error"] == "STALE_PRECONDITION"
    preview_request = connection.send_command.call_args.args[1]
    assert preview_request["migration"]["source"][field] == changed
    assert preview_request["migration"]["source"][field] != reviewed
    connection.send_command_once.assert_not_called()


def test_retirement_closed_component_proofs_are_published_whole():
    connection = MagicMock()
    preview = _preview(5)
    preview["orphan_component_guids"] = _guids(5)
    preview["orphan_component_nodes"] = [f"{guid} exact class and semantic proof" for guid in _guids(5)]
    connection.send_command.return_value = {"success": True, "data": preview}
    request = retire_request(dry_run=True)
    request["migration"]["source"]["orphan_component_node_guids"] = _guids(5)
    payload = _payload(dispatch_graph_apply_patch(connection, request, tool_name="graph_cmd"))
    assert payload["orphan_component_guids"] == preview["orphan_component_guids"]
    assert payload["orphan_component_nodes"] == preview["orphan_component_nodes"]
    assert payload["complete"] is True
    connection.send_command_once.assert_not_called()
