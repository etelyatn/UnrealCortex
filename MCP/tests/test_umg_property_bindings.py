"""Consumer-visible binding safety at the existing UMG router boundary."""

import json
from copy import deepcopy
from unittest.mock import MagicMock

import pytest

from cortex_mcp.pagination import PaginationCache, encode_cursor
from cortex_mcp.response import compact_json, MAX_RESPONSE_CHARS, format_response
from cortex_mcp.tools import routers
from cortex_mcp.tools.routers import make_router


@pytest.mark.parametrize("command,extra", [
    ("set_property_binding", {"binding": None}),
    ("get_tree", {"include_property_bindings": True}),
    ("get_widget", {"include_property_bindings": True}),
])
@pytest.mark.parametrize("field,value", [("cursor", "stale-cursor"), ("cursor", None), ("limit", None), ("offset", None)])
def test_binding_operations_refuse_pagination_instead_of_cached_read(command, extra, field, value):
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": {"unexpected": True}}
    router = make_router("umg", connection, "UMG")
    payload = json.loads(router(command, {"asset_path": "/Game/UI/W", **extra, field: value}))
    assert payload.get("_error") == "INVALID_FIELD"
    connection.send_command.assert_not_called()
    connection.send_command_cached.assert_not_called()


@pytest.mark.parametrize("command", ["get_tree", "get_widget"])
@pytest.mark.parametrize("flag", [None, 0, 1, "true", [], {}])
def test_malformed_inspection_flag_cannot_return_an_unrelated_cached_page(monkeypatch, command, flag):
    cache = PaginationCache()
    monkeypatch.setattr(routers, "_pagination_cache", cache)
    key = cache.store("data.list_datatables", {}, "datatables", [{"name": "DT_Cached"}], {})
    cursor = encode_cursor(key, 0, 1)
    router = make_router("umg", object(), "UMG")
    payload = json.loads(router(command, {
        "asset_path": "/Game/UI/W",
        "widget_name": "ProgressDisplay",
        "include_property_bindings": flag,
        "cursor": cursor,
    }))
    assert payload.get("_error") == "INVALID_FIELD"
    assert "datatables" not in payload


def test_oversized_orphan_inspection_is_explicitly_incomplete_and_retains_guard():
    fingerprint = {"domain_signature": {"scope": "umg.property_binding", "digest": "a" * 64}}
    data = {"root": None, "total_widgets": 0, "property_binding_state": {
        "reader_complete": True, "scope": "asset", "total": 1, "fingerprint": fingerprint,
        "bindings": [{"widget_name": "DeletedWidget", "source_path": ["x" * MAX_RESPONSE_CHARS]}],
        "diagnostics": [],
    }}
    original = deepcopy(data)
    text = format_response(data, "umg_cmd")
    result = json.loads(text)
    assert result["_error"] == "RESPONSE_TOO_LARGE"
    state = result["property_binding_state"]
    assert state["reader_complete"] is False
    assert state["total"] == 1
    assert state["fingerprint"] == fingerprint
    assert "limit" not in result.get("_suggestion", "")
    assert len(text) <= MAX_RESPONSE_CHARS
    assert data == original


def test_oversized_tree_cannot_hide_small_complete_binding_state():
    fingerprint = {"domain_signature": {"scope": "umg.property_binding", "digest": "a" * 64}}
    data = {"root": {"name": "Root", "class": "CanvasPanel", "children": [
        {"name": f"Title_{index}", "class": "TextBlock", "is_variable": False, "children": []}
        for index in range(700)
    ]}, "total_widgets": 701, "property_binding_state": {
        "reader_complete": True, "scope": "asset", "total": 1, "fingerprint": fingerprint,
        "bindings": [{"widget_name": "Title_399", "property_name": "Text", "source_path": []}],
        "diagnostics": [],
    }}
    original = deepcopy(data)
    assert len(compact_json(data)) > MAX_RESPONSE_CHARS
    text = format_response(data, "umg_cmd")
    result = json.loads(text)
    assert result["_error"] == "RESPONSE_TOO_LARGE"
    state = result["property_binding_state"]
    assert state["reader_complete"] is False
    assert state["scope"] == "asset" and state["total"] == 1
    assert state["fingerprint"] == fingerprint
    assert len(text) <= MAX_RESPONSE_CHARS and data == original


def test_oversized_binding_set_preserves_actual_mutation_outcome_not_generic_error():
    fingerprint = {"domain_signature": {"scope": "umg.property_binding", "digest": "a" * 64}}
    data = {"asset_path": "/Game/UI/W", "widget_name": "ProgressDisplay", "property_name": "Percent",
            "changed": True, "compiled": False, "saved": False, "reader_complete": True,
            "fingerprint": fingerprint, "before_binding": {"source_property": "x" * MAX_RESPONSE_CHARS},
            "binding": None}
    result = json.loads(format_response(data, "umg_cmd"))
    assert result["changed"] is True
    assert result["binding"] is None
    assert result["compiled"] is False and result["saved"] is False
    assert result["fingerprint"] == fingerprint
    assert result["reader_complete"] is False
    assert result["_binding_identity_truncated"] is True
    assert "_error" not in result


def test_oversized_replacement_counts_explanation_before_selecting_retained_identity():
    fingerprint = {"domain_signature": {"scope": "umg.property_binding", "digest": "a" * 64}}
    data = {"changed": True, "compiled": False, "saved": False, "reader_complete": True,
            "fingerprint": fingerprint, "before_binding": {"source_path": ["x" * MAX_RESPONSE_CHARS]},
            "binding": {"source_path": [""]}}
    after_first_omission = {
        key: value for key, value in data.items() if key != "before_binding"
    }
    after_first_omission.update(reader_complete=False, _binding_identity_truncated=True,
                               _before_binding_omitted=True)
    available = MAX_RESPONSE_CHARS - len(compact_json(after_first_omission)) - 1
    data["binding"]["source_path"][0] = "y" * available
    original = deepcopy(data)
    text = format_response(data, "umg_cmd")
    result = json.loads(text)
    assert len(text) <= MAX_RESPONSE_CHARS
    assert result["changed"] is True
    assert result["fingerprint"] == fingerprint
    assert result["reader_complete"] is False
    assert "_error" not in result
    assert data == original
