"""A call-output approval cannot use a truncated consumer inventory."""

import json
from unittest.mock import MagicMock

from cortex_mcp.graph_patch_boundary import dispatch_graph_apply_patch
from cortex_mcp.response import MAX_RESPONSE_CHARS, compact_json


EDGE = {"far_node_guid": "11111111-1111-1111-1111-111111111111", "far_pin": "Condition"}


def request(*, dry_run, edges=None):
    migration = {"op": "replace_call_output", "source": {}}
    if edges is not None:
        migration["edges"] = edges
    result = {
        "asset_path": "/Game/Test.WBP_Test",
        "patch_id": "22222222-2222-2222-2222-222222222222",
        "migration": migration,
        "dry_run": dry_run,
    }
    if not dry_run:
        result["expected_validation_hash"] = "token"
    return result


def preview(*, long_path=False):
    return {
        "complete": True,
        "validation_hash": "token",
        "edges": [{**EDGE, "far_pin_signature": "bool", "far_direction": 0, "response": "MAKE"}],
        "locators": {"subgraph_path": "界" * 41_000 if long_path else "Small"},
    }


def test_long_unicode_locator_refuses_whole_preview_instead_of_clipping_edges():
    connection = MagicMock()
    native = preview(long_path=True)
    assert len(compact_json(native)) > MAX_RESPONSE_CHARS
    connection.send_command.return_value = {"success": True, "data": native}

    text = dispatch_graph_apply_patch(connection, request(dry_run=True), tool_name="graph_cmd")
    result = json.loads(text)

    assert result["_error"] == "LIMIT_EXCEEDED"
    assert result["approval_complete"] is False
    assert "edges" not in result
    assert len(text) <= MAX_RESPONSE_CHARS
    connection.send_command_once.assert_not_called()


def test_oversized_consumer_array_refuses_without_a_partial_approval():
    connection = MagicMock()
    native = preview()
    native["edges"] = [
        {**EDGE, "far_node_guid": f"11111111-1111-1111-1111-{i:012d}",
         "far_pin_signature": "bool", "far_direction": 0, "response": "MAKE"}
        for i in range(400)
    ]
    assert len(compact_json(native)) > MAX_RESPONSE_CHARS
    connection.send_command.return_value = {"success": True, "data": native}

    result = json.loads(dispatch_graph_apply_patch(
        connection, request(dry_run=True), tool_name="graph_cmd",
    ))

    assert result["_error"] == "LIMIT_EXCEEDED"
    assert result["approval_complete"] is False
    assert "edges" not in result
    connection.send_command_once.assert_not_called()


def test_oversized_preflight_refuses_apply_before_dispatch():
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": preview(long_path=True)}

    result = json.loads(dispatch_graph_apply_patch(
        connection, request(dry_run=False, edges=[EDGE]), tool_name="graph_cmd",
    ))

    assert result["_error"] == "LIMIT_EXCEEDED"
    connection.send_command_once.assert_not_called()


def test_exact_approved_edges_apply_once_after_complete_preflight():
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": preview()}
    connection.send_command_once.return_value = {"success": True, "data": {"apply_status": "applied"}}

    result = json.loads(dispatch_graph_apply_patch(
        connection, request(dry_run=False, edges=[EDGE]), tool_name="graph_cmd",
    ))

    assert result["apply_status"] == "applied"
    connection.send_command_once.assert_called_once()


def test_partial_approved_edges_refuse_before_dispatch():
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": preview()}

    result = json.loads(dispatch_graph_apply_patch(
        connection, request(dry_run=False, edges=[]), tool_name="graph_cmd",
    ))

    assert result["_error"] == "STALE_PRECONDITION"
    connection.send_command_once.assert_not_called()
