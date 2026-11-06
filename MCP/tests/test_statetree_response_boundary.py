"""Final-review F1/F2: StateTree response boundary for replace_node outcomes and binding pages."""

import json
from unittest.mock import MagicMock

import pytest

from cortex_mcp.response import MAX_RESPONSE_CHARS, compact_json
from cortex_mcp.tcp_client import UECommandError
from cortex_mcp.tools.routers import make_router

FINGERPRINT = {"asset_path": "/Game/AI/ST_Tutorial", "hash": "abc", "editor_content_hash": 12345}


def _router(data: dict):
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": data}
    return make_router("statetree", connection, "statetree docs"), connection


def _applied_bulk_result(rewrites: int = 600, *, property_plan: int = 300, nodes: int = 40) -> dict:
    remapped = [
        {
            "binding_index": i,
            "side": "target",
            "node_struct_side": False,
            "old_path": f"BlueprintProperty{i}.Member",
            "new_path": f"NativeProperty{i}.Member",
            "old_property_guid": "A" * 32,
            "new_property_guid": None,
            "renamed": True,
            "first_segment_array_index": -1,
        }
        for i in range(rewrites)
    ]
    return {
        "schema_version": 1,
        "nodes": [{"node_id": f"N{i}", "name": f"Task{i}", "would_change": True} for i in range(nodes)],
        "property_plan": [
            {"old_property": f"P{i}", "new_property": f"Q{i}", "status": "copied", "old_value": "v" * 40}
            for i in range(property_plan)
        ],
        "bindings": {"before_count": 72, "after_count": 72, "remapped": remapped, "unresolved": []},
        "blueprint_only_members": [],
        "required_acknowledgment": [],
        "type_mismatches": [],
        "would_change": True,
        "unresolved_bindings": [],
        "dry_run": False,
        "replaced": [
            {
                "node_id": f"N{i}",
                "name": f"Task{i}",
                "slot": "task",
                "owner_state_path": "Root/State",
                "old_type_path": "/Game/STT_Old.STT_Old_C",
                "new_type_path": "/Script/Game.Native",
            }
            for i in range(nodes)
        ],
        "compile_status": "success",
        "error_count": 0,
        "ready_to_run": True,
        "saved": True,
        "dirty": False,
        "fingerprint": FINGERPRINT,
    }


class TestReplaceNodeAppliedOutcome:
    def test_premise_payload_exceeds_the_response_budget(self):
        assert len(compact_json(_applied_bulk_result())) > MAX_RESPONSE_CHARS * 3

    def test_oversized_applied_bulk_keeps_the_authoritative_outcome(self):
        router, connection = _router(_applied_bulk_result())

        text = router("replace_node", {"asset_path": "/Game/AI/ST_Tutorial", "blueprint_class": "/Game/X.X_C"})
        payload = json.loads(text)

        assert len(text) <= MAX_RESPONSE_CHARS
        assert payload.get("_error") is None
        assert payload["saved"] is True
        assert payload["compile_status"] == "success"
        assert payload["error_count"] == 0
        assert payload["ready_to_run"] is True
        assert payload["dirty"] is False
        assert payload["fingerprint"] == FINGERPRINT
        assert payload["bindings"]["before_count"] == 72
        assert payload["bindings"]["after_count"] == 72
        assert len(payload["replaced"]) == 40
        assert payload["reader_complete"] is False
        command, _params = connection.send_command.call_args[0][:2]
        assert command == "statetree.replace_node"

    def test_omitted_detail_is_explicit_with_counts(self):
        router, _ = _router(_applied_bulk_result())

        payload = json.loads(router("replace_node", {"asset_path": "/Game/AI/ST_Tutorial", "blueprint_class": "C"}))

        assert "remapped" not in payload["bindings"]
        assert payload["bindings"]["remapped_count"] == 600
        assert payload["bindings"]["_remapped_omitted"] is True
        assert "property_plan" not in payload
        assert payload["property_plan_count"] == 300
        assert "property_plan" in payload["_outcome_detail_omitted"]
        assert "bindings.remapped" in payload["_outcome_detail_omitted"]
        assert payload["_suggestion"]

    def test_replaced_list_larger_than_the_budget_is_counted_not_hidden(self):
        router, _ = _router(_applied_bulk_result(rewrites=10, property_plan=10, nodes=700))

        text = router("replace_node", {"asset_path": "/Game/AI/ST_Tutorial", "blueprint_class": "C"})
        payload = json.loads(text)

        assert len(text) <= MAX_RESPONSE_CHARS
        assert payload.get("_error") is None
        assert payload["saved"] is True
        assert payload["fingerprint"] == FINGERPRINT
        assert payload["replaced_count"] == 700
        assert 0 < len(payload["replaced"]) < 700
        assert payload["replaced_truncated"] is True

    def test_small_applied_outcome_passes_through_unchanged(self):
        data = _applied_bulk_result(rewrites=2, property_plan=2, nodes=1)
        router, _ = _router(data)

        payload = json.loads(router("replace_node", {"asset_path": "/Game/AI/ST_Tutorial", "node_id": "N0"}))

        assert payload == data


def _binding_page(count: int, *, total: int, offset: int = 0, limit: int = 200, has_more: bool = False) -> dict:
    nodes = [
        {
            "node_id": f"N{i}",
            "name": f"Task{i}",
            "as_target": [{"binding_index": j, "path": "x" * 80, "segments": ["s" * 60]} for j in range(12)],
        }
        for i in range(count)
    ]
    return {
        "asset_path": "/Game/AI/ST_Tutorial",
        "selection": "all",
        "direction": "both",
        "nodes": nodes,
        "total": total,
        "offset": offset,
        "limit": limit,
        "returned_count": count,
        "has_more": has_more,
        "whole_tree_binding_count": 72,
        "fingerprint": FINGERPRINT,
    }


class TestNodeBindingsPageOwnership:
    def test_terminal_oversized_page_refuses_instead_of_claiming_completeness(self):
        data = _binding_page(114, total=114)
        assert len(compact_json(data)) > MAX_RESPONSE_CHARS
        router, _ = _router(data)

        payload = json.loads(router("get_node_bindings", {"asset_path": "/Game/AI/ST_Tutorial", "limit": 200}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert payload["_native_page_omitted"] is True
        assert "nodes" not in payload
        assert "_truncated" not in payload
        assert payload["total"] == 114
        assert payload["limit"] == 200
        assert "smaller limit" in payload["_suggestion"].lower()

    def test_nonterminal_oversized_page_refuses_and_hides_has_more_progress(self):
        data = _binding_page(60, total=114, limit=60, has_more=True)
        data["next_offset"] = 60
        assert len(compact_json(data)) > MAX_RESPONSE_CHARS
        router, _ = _router(data)

        payload = json.loads(router("get_node_bindings", {"asset_path": "/Game/AI/ST_Tutorial", "limit": 60}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["_native_page_omitted"] is True
        assert "nodes" not in payload
        assert "next_offset" not in payload
        assert payload["offset"] == 0

    def test_fitting_page_is_returned_whole_without_python_cursor(self):
        data = _binding_page(3, total=3)
        data["nodes"] = [{"node_id": f"N{i}"} for i in range(3)]
        router, connection = _router(data)

        payload = json.loads(router("get_node_bindings", {"asset_path": "/Game/AI/ST_Tutorial", "limit": 3}))

        assert payload == data
        assert connection.send_command.call_args[0][1]["limit"] == 3

    def test_native_limit_exceeded_contract_is_preserved(self):
        connection = MagicMock()
        connection.send_command.side_effect = UECommandError(
            "statetree.get_node_bindings", "LIMIT_EXCEEDED", "limit 201 exceeds the maximum of 200", {"max_limit": 200}
        )
        router = make_router("statetree", connection, "statetree docs")

        payload = json.loads(router("get_node_bindings", {"asset_path": "/Game/AI/ST_Tutorial", "limit": 201}))

        assert payload["_error"] == "LIMIT_EXCEEDED"
        assert connection.send_command.call_args[0][1]["limit"] == 201

    def test_python_cursor_is_refused_before_dispatch(self):
        router, connection = _router(_binding_page(1, total=1))

        payload = json.loads(router("get_node_bindings", {"asset_path": "/Game/AI/ST_Tutorial", "cursor": "x"}))

        assert payload["_error"] == "INVALID_CURSOR"
        connection.send_command.assert_not_called()
