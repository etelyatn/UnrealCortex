"""Task 3 contract: explicit native/cached pagination ownership, no cursor skipping.

Every assertion is consumer-visible at the Python boundary. Native limits/offsets are asserted
only as *forwarding* at this boundary; a mocked native window is never evidence that a native
limit was honoured. The real native page traces and the source ownership table are controller-owned
(see task-3-report.md).

Retained here: behaviour reconstruction (contiguous pages, no skipped rows), cursor
mismatch/expiry/malformed refusals, complete-or-refuse selector policies, and the protected
authoring branch. Wiring-only constant copies, raw cursor encoding pins and cache-entry internals
are deliberately not asserted.
"""

import base64
import json
from unittest.mock import MagicMock

import pytest

from cortex_mcp import response as response_mod
from cortex_mcp.response import MAX_RESPONSE_CHARS, compact_json, format_response
from cortex_mcp.tcp_client import UECommandError
from cortex_mcp.tools import routers as routers_mod
from cortex_mcp.tools.routers import make_router


def _router(domain: str, data: dict | None = None):
    connection = MagicMock()
    response = {"success": True, "data": data if data is not None else {}}
    connection.send_command.return_value = response
    connection.send_command_cached.return_value = response
    return make_router(domain, connection, f"{domain} docs"), connection


def _actors(count: int, *, blob: int = 0):
    return [
        {
            "name": f"Actor_{i}",
            "object_path": f"/Game/Maps/Test.Test:PersistentLevel.Actor_{i}",
            **({"blob": "x" * blob} if blob else {}),
        }
        for i in range(count)
    ]


@pytest.fixture(autouse=True)
def _clear_pagination_cache():
    routers_mod._pagination_cache.clear()
    yield
    routers_mod._pagination_cache.clear()


class TestNativeOwnership:
    """Native-owned pages receive their selector/window untouched and add no Python cursor."""

    def test_query_datatable_forwards_limit_and_offset_without_python_cursor(self):
        # The native window for limit=60 offset=7 over an 80-row table. This double asserts the
        # Python boundary only: it is not proof that the native limit was honoured.
        window = [{"name": f"Row_{i}"} for i in range(7, 67)]
        data = {"rows": window, "total_count": 80, "offset": 7, "limit": 60}
        router, connection = _router("data", data)

        payload = json.loads(router("query_datatable", {"table_path": "/Game/DT_Test", "limit": 60, "offset": 7}))

        assert len(payload["rows"]) == 60
        assert payload["rows"] == window
        assert payload["total_count"] == 80
        assert payload["offset"] == 7
        assert payload["limit"] == 60
        assert "_pagination" not in payload
        assert "_truncated" not in payload
        command, params = connection.send_command.call_args[0]
        assert command == "data.query_datatable"
        assert params == {"table_path": "/Game/DT_Test", "limit": 60, "offset": 7}

    def test_list_actors_accepts_limit_above_python_cache_max(self):
        data = {"actors": [{"name": "A_0"}], "total_count": 150, "offset": 0, "limit": 150}
        router, connection = _router("level", data)

        payload = json.loads(router("list_actors", {"limit": 150}))

        assert payload.get("_error") != "INVALID_LIMIT"
        assert "_pagination" not in payload
        assert connection.send_command.call_args[0][1]["limit"] == 150

    def test_editor_cvar_limit_500_is_native_owned(self):
        data = {"variables": [{"name": "t.Var"}], "returned_count": 1, "total_matched": 500}
        router, connection = _router("editor", data)

        payload = json.loads(router("list_cvars", {"pattern": "t.", "limit": 500}))

        assert payload.get("_error") != "INVALID_LIMIT"
        assert "_pagination" not in payload
        assert connection.send_command.call_args[0][1]["limit"] == 500

    def test_gen_list_jobs_limit_zero_is_native_unlimited(self):
        data = {"jobs": [{"job_id": "j1"}]}
        router, connection = _router("gen", data)

        payload = json.loads(router("list_jobs", {"limit": 0}))

        assert payload.get("_error") != "INVALID_LIMIT"
        assert "_pagination" not in payload
        assert connection.send_command.call_args[0][1]["limit"] == 0

    def test_native_command_refuses_python_cursor_before_dispatch(self):
        router, connection = _router("data", {"rows": []})

        payload = json.loads(router("query_datatable", {"table_path": "/Game/DT_Test", "cursor": "opaque"}))

        assert payload["_error"] == "INVALID_CURSOR"
        assert "cursor" in payload["_message"].lower()
        connection.send_command.assert_not_called()

    def test_oversize_native_page_refuses_without_advancing_continuation(self):
        rows = [{"name": f"Row_{i}", "payload": "x" * 400} for i in range(200)]
        # A real native page advertises its own continuation; the refusal must not republish it.
        data = {
            "rows": rows,
            "total_count": 500,
            "offset": 0,
            "limit": 200,
            "next_offset": 200,
            "next_cursor": "opaque-native-continuation",
            "is_complete": False,
        }
        router, _ = _router("data", data)

        payload = json.loads(router("query_datatable", {"table_path": "/Game/DT_Test", "limit": 200}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert payload["_native_page_omitted"] is True
        assert "rows" not in payload
        assert "_truncated" not in payload
        assert payload["offset"] == 0
        assert payload["limit"] == 200
        assert payload["total_count"] == 500
        assert "next_offset" not in payload
        assert "next_cursor" not in payload
        assert "pagination" not in payload
        assert payload["_suggestion"]
        assert "smaller limit" in payload["_suggestion"].lower()
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS

    @pytest.mark.parametrize(
        "command,params",
        [
            ("set_text", {"asset_path": "/Game/WBP_Test", "widget_name": "Target", "text": "new text"}),
            ("set_font", {"asset_path": "/Game/WBP_Test", "widget_name": "Target", "size": 12}),
            (
                "set_padding",
                {
                    "asset_path": "/Game/WBP_Test",
                    "widget_name": "Target",
                    "padding": {"left": 0, "top": 1, "right": 0, "bottom": 0},
                },
            ),
            ("set_anchor", {"asset_path": "/Game/WBP_Test", "widget_name": "Target", "preset": "Center"}),
            ("set_alignment", {"asset_path": "/Game/WBP_Test", "widget_name": "Target", "horizontal": "Center"}),
            ("set_size", {"asset_path": "/Game/WBP_Test", "widget_name": "Target", "width": 10}),
        ],
    )
    def test_strict_setter_keeps_an_undeclared_limit_for_the_native_validator(self, command, params):
        """Python pagination keys are consumed only by page owners; every other command keeps its
        parameters verbatim, so the native declared-contract validator can refuse them."""
        router, connection = _router("umg", {})

        router(command, {**params, "limit": 1})

        forwarded = connection.send_command.call_args.args[1]
        assert forwarded == {**params, "limit": 1}

    def test_umg_native_page_owner_forwards_its_own_declared_limit(self):
        """umg.list_animation_bindings owns its page in C++; the Python layer must not eat its limit."""
        data = {
            "bindings": [],
            "pagination": {"total": 0, "offset": 0, "limit": 5, "returned": 0, "next_offset": None, "is_complete": True},
        }
        router, connection = _router("umg", data)

        router("list_animation_bindings", {"asset_path": "/Game/WBP_Test", "limit": 5})

        forwarded = connection.send_command.call_args.args[1]
        assert forwarded["limit"] == 5

    def test_umg_animation_bindings_summary_read_is_complete_or_refuse(self):
        """An oversized native summary page must refuse explicitly — never be shortened here.

        The C++ handler computes next_offset/is_complete for the whole page, so a Python-shortened
        page would carry a continuation that does not describe the rows actually returned.
        """
        bindings = [
            {"widget_name": f"W_{i}", "property_path": "ColorAndOpacity", "blob": "x" * 400}
            for i in range(200)
        ]
        data = {
            "asset_path": "/Game/WBP_Test",
            "bindings": bindings,
            "summary_counts": {"umg_binding_count": 200, "movie_scene_binding_count": 0, "track_count": 0},
            "pagination": {"total": 200, "offset": 0, "limit": 200, "returned": 200, "next_offset": None, "is_complete": True},
        }
        router, _ = _router("umg", data)

        payload = json.loads(
            router("list_animation_bindings", {"asset_path": "/Game/WBP_Test", "limit": 200})
        )

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["_native_page_omitted"] is True
        assert payload["reader_complete"] is False
        assert "_truncated" not in payload
        assert "bindings" not in payload
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS

    def test_umg_animation_bindings_detail_read_keeps_the_detailed_envelope(self):
        """The dedicated detailed-read handling must survive the summary-path change."""
        bindings = [
            {
                "widget_name": f"W_{i}",
                "property_path": "ColorAndOpacity",
                "tracks": [{"keys": [{"time": 0.0, "value": "x" * 400}]}],
            }
            for i in range(200)
        ]
        data = {
            "asset_path": "/Game/WBP_Test",
            "animation_name": "FadeIn",
            "fingerprint": {"domain_signature": {"scope": "umg.property_binding"}},
            "summary_counts": {"umg_binding_count": 200, "movie_scene_binding_count": 3, "track_count": 5},
            "bindings": bindings,
        }
        router, _ = _router("umg", data)

        payload = json.loads(
            router(
                "list_animation_bindings",
                {"asset_path": "/Game/WBP_Test", "include_track_content": True},
            )
        )

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert "summary_counts" in payload
        assert "_native_page_omitted" not in payload

    def test_native_error_keeps_qualified_command_and_no_python_pagination(self):
        connection = MagicMock()
        connection.send_command.side_effect = UECommandError(
            "data.query_datatable", "BAD_QUERY", "query failed", {"offset": 7}
        )
        router = make_router("data", connection, "data docs")

        payload = json.loads(router("query_datatable", {"table_path": "/Game/DT_Test", "limit": 60, "offset": 7}))

        assert payload["success"] is False
        assert payload["_error"] == "BAD_QUERY"
        assert payload["_command"] == "data.query_datatable"
        assert "_pagination" not in payload


class TestFormatNativePage:
    """format_native_page is complete-or-refuse; it never shortens a native page."""

    def test_complete_native_page_is_returned_verbatim(self):
        data = {"rows": [{"name": "Row_0"}], "total_count": 1, "offset": 0, "limit": 1}

        text = response_mod.format_native_page(data, "data.query_datatable", {"table_path": "/Game/DT_Test"})

        assert text == compact_json(data)

    def test_single_oversized_record_refuses_honestly(self):
        data = {"rows": [{"name": "Row_0", "payload": "x" * 60_000}], "total_count": 1, "offset": 0, "limit": 1}

        text = response_mod.format_native_page(data, "data.query_datatable", {"table_path": "/Game/DT_Test"})
        payload = json.loads(text)

        assert len(text) <= MAX_RESPONSE_CHARS
        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert "rows" not in payload
        assert "_truncated" not in payload
        assert "next_offset" not in payload
        assert payload["qualified_command"] == "data.query_datatable"


class TestCachedPages:
    """Cache-owned pages honour small requests and never skip rows on the next cursor."""

    def test_seven_items_requested_limit_two_yield_four_contiguous_pages(self):
        router, connection = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})

        seen = []
        payload = json.loads(router("find_actors", {"limit": 2}))
        while True:
            assert payload["_pagination"]["total"] == 7
            assert payload["_pagination"]["returned"] == len(payload["matches"])
            seen.extend(item["name"] for item in payload["matches"])
            cursor = payload["_pagination"]["next_cursor"]
            if cursor is None:
                break
            payload = json.loads(router("find_actors", {"cursor": cursor}))

        assert seen == [f"Actor_{i}" for i in range(7)]
        assert connection.send_command.call_count == 1

    def test_oversized_cached_page_next_cursor_does_not_skip_rows(self):
        router, _ = _router("level", {"matches": _actors(20, blob=3000), "count": 20, "pattern": "Actor"})

        first = json.loads(router("find_actors", {"limit": 20}))

        assert len(compact_json(first)) <= MAX_RESPONSE_CHARS
        returned = len(first["matches"])
        assert 0 < returned < 20
        assert first["_pagination"]["returned"] == returned
        cursor = first["_pagination"]["next_cursor"]
        assert cursor is not None

        second = json.loads(router("find_actors", {"cursor": cursor}))

        assert second["matches"], "the continuation must return the rows the first page omitted"
        assert second["matches"][0]["name"] == f"Actor_{returned}"
        assert second["_pagination"]["returned"] == len(second["matches"])

    def test_single_oversized_cached_item_refuses_without_advancing_cursor(self):
        router, _ = _router("level", {"matches": _actors(2, blob=50_000), "count": 2, "pattern": "Actor"})

        payload = json.loads(router("find_actors", {"limit": 2}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert payload.get("_pagination") is None
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS

    def test_cursor_from_another_command_refuses_before_dispatch(self):
        router, connection = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})
        first = json.loads(router("find_actors", {"limit": 2}))
        cursor = first["_pagination"]["next_cursor"]

        connection.send_command.reset_mock()
        other = make_router("material", connection, "material docs")
        payload = json.loads(other("list_materials", {"cursor": cursor}))

        assert payload["_error"] == "INVALID_CURSOR"
        connection.send_command.assert_not_called()

    def test_cursor_for_unmapped_read_refuses_before_dispatch(self):
        router, _ = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})
        cursor = json.loads(router("find_actors", {"limit": 2}))["_pagination"]["next_cursor"]

        unmapped, connection = _router("data", {"fields": []})
        payload = json.loads(unmapped("get_datatable_schema", {"table_path": "/Game/DT_Test", "cursor": cursor}))

        assert payload["_error"] == "INVALID_CURSOR"
        connection.send_command.assert_not_called()

    def test_expired_cursor_refuses(self):
        router, _ = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})
        cursor = json.loads(router("find_actors", {"limit": 2}))["_pagination"]["next_cursor"]

        routers_mod._pagination_cache.clear()
        payload = json.loads(router("find_actors", {"cursor": cursor}))

        assert payload["_error"] == "CURSOR_EXPIRED"

    def test_malformed_cursor_refuses(self):
        router, connection = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})

        payload = json.loads(router("find_actors", {"cursor": "!!!not-base64!!!"}))

        assert payload["_error"] == "INVALID_CURSOR"
        connection.send_command.assert_not_called()

    def test_mixed_node_and_edge_read_is_never_cursor_paginated(self):
        data = {
            "nodes": [{"node_id": f"n{i}"} for i in range(20)],
            "node_count": 20,
            "edges": [{"from": "n0", "to": "n1"}],
        }
        router, connection = _router("graph", data)

        payload = json.loads(
            router("get_subgraph", {"asset_path": "/Game/BP_Test", "graph_name": "EventGraph", "limit": 5})
        )

        assert "_pagination" not in payload
        assert payload["nodes"] == data["nodes"]
        assert payload["edges"] == data["edges"]
        command, _params = connection.send_command.call_args[0]
        assert command == "graph.get_subgraph"


def _huge_fields(prefix: str, count: int = 200, size: int = 400) -> dict:
    return {f"{prefix}{i}": "x" * size for i in range(count)}


class TestCompleteOrRefuseReadPolicies:
    """Reads with their own selector refuse oversize with that selector, never a generic cut."""

    def test_data_asset_oversize_read_refuses_naming_fields_selector(self):
        data = {"asset_path": "/Game/DA_Test", "class_name": "TestDataAsset", "properties": _huge_fields("Field_")}
        router, _ = _router("data", data)

        payload = json.loads(router("get_data_asset", {"asset_path": "/Game/DA_Test"}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert payload["asset_path"] == "/Game/DA_Test"
        assert payload.get("properties") is None
        assert "_truncated" not in payload
        assert "fields" in payload["_suggestion"].lower()
        assert "limit" not in payload["_suggestion"].lower()

    def test_material_node_oversize_read_refuses_naming_properties_selector(self):
        # Native material.get_node returns node_id/expression_class/position/properties only: the
        # asset selector exists only in the caller's request and must survive the refusal.
        data = {
            "node_id": "Constant_1",
            "expression_class": "MaterialExpressionConstant",
            "position": {"x": 10, "y": 20},
            "properties": {f"P{i}": {"value": "x" * 200} for i in range(300)},
        }
        router, _ = _router("material", data)

        payload = json.loads(router("get_node", {"asset_path": "/Game/M_Test", "node_id": "Constant_1"}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert payload["asset_path"] == "/Game/M_Test"
        assert payload["node_id"] == "Constant_1"
        assert payload.get("properties") is None
        assert "_truncated" not in payload
        assert "properties" in payload["_suggestion"].lower()
        assert "limit" not in payload["_suggestion"].lower()
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS

    def test_umg_tree_oversize_read_refuses_naming_tree_selectors(self):
        # Native umg.get_tree returns root/total_widgets only — no asset_path — so the caller's
        # selector has to survive the refusal from the request, not from the native echo.
        children = [
            {"name": f"T3_{index:04d}_" + "W" * 96, "class": "TextBlock", "children": []}
            for index in range(420)
        ]
        data = {
            "root": {"name": "T3Root", "class": "CanvasPanel", "children": children},
            "total_widgets": 421,
        }
        router, _ = _router("umg", data)

        payload = json.loads(router("get_tree", {"asset_path": "/Game/WBP_Native"}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert payload["asset_path"] == "/Game/WBP_Native"
        assert payload["total_widgets"] == 421
        assert payload.get("root") is None
        assert "_truncated" not in payload
        suggestion = payload["_suggestion"].lower()
        assert "depth" in suggestion or "max_nodes" in suggestion or "names_only" in suggestion
        assert "get_widget" in suggestion or "names_only" in suggestion
        assert "limit" not in suggestion
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS

    def test_graph_subgraph_explicit_node_read_is_never_silently_truncated(self):
        # graph.get_subgraph selects explicit node_ids and reads a mixed node+edge topology. An
        # oversized read must refuse naming that selector, and a fitting read must return every
        # requested node — either way the node array is never silently shortened, and the generic
        # largest-array truncator (which would emit `_truncated`) must never own this read.
        node_ids = [f"n{i}" for i in range(30)]
        request = {"asset_path": "/Game/BP_Test", "graph_name": "EventGraph", "node_ids": node_ids}

        oversized = {
            "nodes": [{"node_id": nid, "title": "N" * 2000} for nid in node_ids],
            "node_count": 30,
            "edges": [{"from": "n0", "to": "n1"}],
        }
        router, _ = _router("graph", oversized)

        payload = json.loads(router("get_subgraph", request))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert "nodes" not in payload
        assert "_truncated" not in payload
        assert payload["asset_path"] == "/Game/BP_Test"
        assert payload["graph_name"] == "EventGraph"
        suggestion = payload["_suggestion"]
        assert "node_ids" in suggestion or "include_edges" in suggestion
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS

        fitting = {
            "nodes": [{"node_id": nid, "title": f"Node {nid}"} for nid in node_ids],
            "node_count": 30,
            "edges": [{"from": "n0", "to": "n1"}],
        }
        router, _ = _router("graph", fitting)

        payload = json.loads(router("get_subgraph", request))

        assert "_error" not in payload
        assert payload["nodes"] == fitting["nodes"]
        assert set(node_ids) <= {node["node_id"] for node in payload["nodes"]}
        assert "_truncated" not in payload
        assert payload.get("reader_complete") is not False

    def test_blueprint_defaults_oversize_refuses_naming_discovery_selectors(self):
        data = {
            "asset_path": "/Game/BP_Test",
            "properties": {f"P{i}": {"type": "float", "value": "x" * 200} for i in range(300)},
        }

        payload = json.loads(
            format_response(
                data,
                "blueprint_cmd",
                qualified_command="blueprint.get_class_defaults",
                request_params={"asset_path": "/Game/BP_Test"},
            )
        )

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert payload.get("properties") is None
        assert "_truncated" not in payload
        suggestion = payload["_suggestion"].lower()
        assert "properties" in suggestion
        assert "include_values" in suggestion or "discovery_limit" in suggestion
        assert "limit" not in suggestion.replace("discovery_limit", "")

    def test_core_capability_facade_oversize_refuses_naming_detail_names(self):
        data = {"detail": "full", "operations": {f"op_{i}": {"schema": "x" * 400} for i in range(200)}}

        payload = json.loads(
            format_response(
                data,
                "core_cmd",
                qualified_command="get_capabilities",
                request_params={"command": "get_capabilities"},
            )
        )

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert "_truncated" not in payload
        suggestion = payload["_suggestion"].lower()
        assert "detail=names" in suggestion or ("detail" in suggestion and "names" in suggestion)

    def test_data_catalog_oversize_refuses_naming_section_selectors(self):
        data = {
            "datatables": [
                {"path": f"/Game/DT_{i}", "class_name": "X" * 60, "count": i} for i in range(400)
            ]
        }

        payload = json.loads(
            format_response(
                data,
                "data_cmd",
                qualified_command="data.get_data_catalog",
                request_params={},
            )
        )

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert "_truncated" not in payload
        suggestion = payload["_suggestion"].lower()
        assert "sections" in suggestion
        assert "include_asset_paths" in suggestion

    def test_reflect_detail_oversize_refuses_naming_summary_detail(self):
        data = {
            "class_name": "AActor",
            "properties": {f"P{i}": {"type": "float", "value": "x" * 200} for i in range(300)},
        }

        payload = json.loads(format_response(data, "query_class_detail", request_params={"class_name": "AActor"}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert "_truncated" not in payload
        assert "detail=summary" in payload["_suggestion"].lower()

    def test_reflect_context_oversize_refuses_naming_summary_or_properties(self):
        data = {
            "class_name": "AActor",
            "parent": {"name": "UObject", "properties": _huge_fields("P_")},
            "children": [{"name": f"C{i}", "class": "X" * 80} for i in range(400)],
        }

        payload = json.loads(
            format_response(data, "query_class_context", request_params={"class_name": "AActor"})
        )

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert "_truncated" not in payload
        suggestion = payload["_suggestion"].lower()
        assert "detail=summary" in suggestion or "properties" in suggestion

    def test_fitting_read_is_never_policed(self):
        data = {"asset_path": "/Game/DA_Test", "properties": {"Small": 1}}

        text = format_response(
            data,
            "data_cmd",
            qualified_command="data.get_data_asset",
            request_params={"asset_path": "/Game/DA_Test"},
        )

        assert json.loads(text) == data

    def test_error_envelope_is_never_converted_into_a_read_refusal(self):
        data = {
            "success": False,
            "_error": "UNKNOWN",
            "_message": "native failure " + "x" * 50_000,
            "asset_path": "/Game/DA_Test",
        }

        payload = json.loads(
            format_response(
                data,
                "ue_command_error",
                command="get_data_asset",
                qualified_command="data.get_data_asset",
                request_params={"asset_path": "/Game/DA_Test"},
            )
        )

        # A native error is never rewritten into a selector refusal: the read policy only shapes
        # successful reads, and a selector suggestion would hide the actual failure.
        assert payload.get("reader_complete") is not False
        assert "fields" not in payload.get("_suggestion", "").lower()

    def test_protected_animation_authoring_branch_wins_over_generic_policy(self):
        data = {
            "asset_path": "/Game/WBP_Test",
            "animation_name": "Anim_0",
            "changed": True,
            "saved": True,
            "authored_track": {"keys": [{"time": i, "value": "x" * 200} for i in range(300)]},
        }

        payload = json.loads(
            format_response(
                data,
                "umg_cmd",
                command="set_animation_property_track",
                qualified_command="umg.set_animation_property_track",
                request_params={"asset_path": "/Game/WBP_Test"},
            )
        )

        assert payload["changed"] is True
        assert payload.get("reader_complete") is False
        assert payload.get("authored_track_omitted") is True
        assert "_error" not in payload


class TestRefusalContextBudget:
    """A refusal is always under the budget and reports every omitted selector explicitly.

    The refusal never shortens the native page. When a selector/echo value is itself too large to
    carry, it is omitted and named in `_omitted_context`, while the exact identity and window
    scalars the caller needs to re-issue the request survive.
    """

    BIG = "x" * 50_000

    def test_native_refusal_with_oversized_native_selector_stays_within_budget(self):
        # Real shape of editor.list_cvars with a 50000-character pattern and no matching variables.
        data = {
            "variables": [],
            "commands": [],
            "returned_count": 0,
            "total_matched": 0,
            "pattern": self.BIG,
        }
        router, _ = _router("editor", data)

        payload = json.loads(router("list_cvars", {"pattern": self.BIG, "limit": 500}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert payload["_native_page_omitted"] is True
        assert "pattern" not in payload
        assert payload["_omitted_context"] == ["pattern"]
        assert payload["returned_count"] == 0
        assert payload["total_matched"] == 0
        assert payload["limit"] == 500
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS

    def test_format_native_page_refusal_never_exceeds_budget(self):
        data = {"rows": [], "pattern": self.BIG, "returned_count": 0, "total_matched": 0}

        text = response_mod.format_native_page(data, "editor.list_cvars", {"pattern": self.BIG, "limit": 500})
        payload = json.loads(text)

        assert len(text) <= MAX_RESPONSE_CHARS
        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["_omitted_context"] == ["pattern"]
        assert payload["limit"] == 500

    def test_refusal_without_oversized_context_reports_no_omission(self):
        rows = [{"name": f"Row_{i}", "payload": "x" * 400} for i in range(200)]
        data = {"rows": rows, "total_count": 500, "offset": 0, "limit": 200}

        payload = json.loads(
            response_mod.format_native_page(data, "data.query_datatable", {"limit": 200})
        )

        assert "_omitted_context" not in payload
        assert payload["limit"] == 200

    def test_selector_refusal_omits_oversized_native_scalar_explicitly(self):
        data = {
            "asset_path": "/Game/DA_Native",
            "asset_class": "C" * 60_000,
            "properties": _huge_fields("Field_"),
        }
        router, _ = _router("data", data)

        payload = json.loads(router("get_data_asset", {"asset_path": "/Game/DA_Native"}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert payload["asset_path"] == "/Game/DA_Native"
        assert "asset_class" not in payload
        assert payload["_omitted_context"] == ["asset_class"]
        assert "fields" in payload["_suggestion"].lower()
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS

    def test_native_refusal_reports_the_authoritative_applied_window(self):
        # level.list_actors clamps offset>=0 and the cap (1..1000) and emits the APPLIED window, so
        # the refusal must report 1000/0 instead of the unconsumed request 5000/-7.
        rows = [{"name": f"Actor_{i}", "payload": "x" * 400} for i in range(1000)]
        data = {"actors": rows, "count": 1000, "total": 5000, "offset": 0, "limit": 1000}
        router, _ = _router("level", data)

        payload = json.loads(router("list_actors", {"limit": 5000, "offset": -7}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert payload["limit"] == 1000
        assert payload["offset"] == 0
        assert payload["total"] == 5000
        assert "actors" not in payload
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS


def _retag_cursor(cursor: str, **fields) -> str:
    """Rewrite an opaque cursor's self-declared provenance (forgery probe)."""
    raw = json.loads(base64.b64decode(cursor))
    raw.update(fields)
    return base64.b64encode(json.dumps(raw).encode()).decode()


class TestCachedPageIntegrity:
    """An empty mapped population is a normal empty page; forged provenance refuses."""

    @pytest.mark.parametrize(
        ("domain", "command", "array_key", "data"),
        [
            ("level", "find_actors", "matches", {"matches": [], "count": 0, "pattern": "Nothing"}),
            ("data", "list_datatables", "datatables", {"datatables": [], "count": 0}),
        ],
    )
    def test_empty_mapped_population_returns_a_terminal_empty_page(self, domain, command, array_key, data):
        router, connection = _router(domain, data)

        payload = json.loads(router(command, {"limit": 2}))

        assert payload[array_key] == []
        assert payload["_pagination"]["returned"] == 0
        assert payload["_pagination"]["total"] == 0
        assert payload["_pagination"]["has_more"] is False
        assert payload["_pagination"]["next_cursor"] is None
        assert connection.send_command.call_count == 1
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS

    def test_cursor_retagged_to_another_command_refuses_without_dispatch(self):
        level_router, _ = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})
        cursor = json.loads(level_router("find_actors", {"limit": 2}))["_pagination"]["next_cursor"]
        forged = _retag_cursor(cursor, command="material.list_materials", array="materials")

        material_router, connection = _router("material", {"materials": [{"name": "M_Cached"}]})
        payload = json.loads(material_router("list_materials", {"cursor": forged}))

        assert payload["_error"] == "INVALID_CURSOR"
        assert "Actor_" not in compact_json(payload)
        connection.send_command.assert_not_called()

    def test_cursor_with_wrong_declared_array_refuses(self):
        router, connection = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})
        cursor = json.loads(router("find_actors", {"limit": 2}))["_pagination"]["next_cursor"]
        forged = _retag_cursor(cursor, array="materials")

        # The first page legitimately dispatched; only the forged-cursor call must not.
        connection.send_command.reset_mock()
        payload = json.loads(router("find_actors", {"cursor": forged}))

        assert payload["_error"] == "INVALID_CURSOR"
        connection.send_command.assert_not_called()

    @pytest.mark.parametrize("bad_limit", [0, -1, 5000])
    def test_cursor_with_out_of_range_embedded_limit_refuses(self, bad_limit):
        # A minted cursor always carries a 1..200 limit. A forged 0/negative limit slices an empty
        # page while rows remain, so it must refuse instead of reporting a complete empty read.
        router, connection = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})
        cursor = json.loads(router("find_actors", {"limit": 2}))["_pagination"]["next_cursor"]
        forged = _retag_cursor(cursor, limit=bad_limit)

        connection.send_command.reset_mock()
        payload = json.loads(router("find_actors", {"cursor": forged}))

        assert payload["_error"] == "INVALID_CURSOR"
        assert "matches" not in payload
        assert "_pagination" not in payload
        connection.send_command.assert_not_called()


class TestRefusalDiagnostics:
    """A selector refusal never silently loses native issue/diagnostic context."""

    def test_small_native_diagnostics_are_retained(self):
        data = {
            "asset_path": "/Game/DA_Diag",
            "asset_class": "CortexTestDataAsset",
            "partial": True,
            "issues": [{"path": "TestProperty", "message": "unsupported export"}],
            "warnings": ["serialization warning"],
            "properties": _huge_fields("Field_"),
        }
        router, _ = _router("data", data)

        payload = json.loads(router("get_data_asset", {"asset_path": "/Game/DA_Diag"}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["partial"] is True
        assert payload["issues"] == [{"path": "TestProperty", "message": "unsupported export"}]
        assert payload["warnings"] == ["serialization warning"]
        assert "_omitted_context" not in payload
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS

    def test_oversized_native_diagnostics_are_omitted_explicitly(self):
        data = {
            "asset_path": "/Game/DA_Diag",
            "partial": True,
            "issues": [{"path": "TestProperty", "message": "x" * 60_000}],
            "properties": _huge_fields("Field_"),
        }
        router, _ = _router("data", data)

        payload = json.loads(router("get_data_asset", {"asset_path": "/Game/DA_Diag"}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["partial"] is True
        assert payload["issues_count"] == 1
        assert "issues" not in payload
        assert "issues" in payload["_omitted_context"]
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS


# --------------------------------------------------------------------------------------
# #120 D4/D5: batch_query pagination over one execution, snapshot-owned cursors
# --------------------------------------------------------------------------------------


class _CountingConnection:
    """Fake editor connection that counts native sends and answers `batch` with N step results."""

    def __init__(self, step_count: int, *, blob: int = 0):
        self.sends: list[tuple[str, dict]] = []
        self._results = [
            {"index": i, "success": True, "data": {"name": f"Step_{i}", **({"blob": "x" * blob} if blob else {})}}
            for i in range(step_count)
        ]

    def send_command(self, command, params=None, **_kwargs):
        self.sends.append((command, params))
        return {
            "success": True,
            "data": {"results": list(self._results), "count": len(self._results), "total_timing_ms": 3},
        }


def _batch_steps(count: int):
    return [{"command": "data.list_datatables", "params": {"i": i}} for i in range(count)]


def _core_router(connection):
    return routers_mod.strict_router_tool(make_router("core", connection, "core docs"), "core")


def _drain_batch(router, first: dict):
    payload = first
    seen = []
    while True:
        seen.extend(step["index"] for step in payload["results"])
        cursor = payload["_pagination"]["next_cursor"]
        if cursor is None:
            return seen, payload
        payload = json.loads(router("batch_query", {"cursor": cursor}))


class TestBatchQueryPagination:
    def test_limit_then_cursor_pages_return_every_step_once_with_one_send(self):
        connection = _CountingConnection(25)
        router = _core_router(connection)

        first = json.loads(router("batch_query", {"commands": _batch_steps(25), "limit": 10}))

        assert first["_pagination"]["total"] == 25
        assert first["_pagination"]["returned"] == 10
        assert first["_pagination"]["has_more"] is True
        assert first["count"] == 25
        seen, last = _drain_batch(router, first)
        assert seen == list(range(25))
        assert last["_pagination"]["has_more"] is False
        assert len(connection.sends) == 1
        command, params = connection.sends[0]
        assert command == "batch"
        assert "limit" not in params and "cursor" not in params

    def test_cursor_page_needs_no_commands_and_never_reexecutes(self):
        connection = _CountingConnection(6)
        router = _core_router(connection)
        first = json.loads(router("batch_query", {"commands": _batch_steps(6), "limit": 4}))

        second = json.loads(router("batch_query", {"cursor": first["_pagination"]["next_cursor"]}))

        assert [step["index"] for step in second["results"]] == [4, 5]
        assert len(connection.sends) == 1

    def test_limit_preserves_zero_command_refusal(self):
        connection = _CountingConnection(1)
        router = _core_router(connection)

        payload = json.loads(router("batch_query", {"commands": [], "limit": 5}))

        assert payload["_error"] == "INVALID_INVOCATION_SHAPE"
        assert connection.sends == []

    def test_limit_preserves_apply_patch_refusal(self):
        connection = _CountingConnection(1)
        router = _core_router(connection)
        steps = _batch_steps(1) + [{"command": "graph.apply_patch", "params": {}}]

        payload = json.loads(router("batch_query", {"commands": steps, "limit": 5}))

        assert payload["_error"] == "INVALID_OPERATION"
        assert connection.sends == []

    def test_invalid_limit_refuses_before_dispatch(self):
        connection = _CountingConnection(3)
        router = _core_router(connection)

        payload = json.loads(router("batch_query", {"commands": _batch_steps(3), "limit": 0}))

        assert payload["_error"] == "INVALID_LIMIT"
        assert connection.sends == []

    def test_oversized_batch_without_limit_serves_executed_results_by_cursor(self):
        connection = _CountingConnection(30, blob=3_000)
        router = _core_router(connection)

        first = json.loads(router("batch_query", {"commands": _batch_steps(30)}))

        assert "_error" not in first
        assert len(compact_json(first)) <= MAX_RESPONSE_CHARS
        pagination = first["_pagination"]
        assert pagination["total"] == 30
        assert pagination["has_more"] is True
        assert pagination["next_cursor"]
        assert pagination["snapshot_id"] >= 1
        assert 0 < pagination["returned"] < 30
        assert [step["index"] for step in first["results"]] == list(range(pagination["returned"]))
        # The caller is told the batch already ran and must not be re-run.
        assert "already executed" in first["_note"]
        assert "do not re-run" in first["_note"].lower()
        assert "cursor" in first["_note"]

        # Every executed result stays recoverable through the cursors, with no second execution.
        seen, last = _drain_batch(router, first)
        assert seen == list(range(30))
        assert last["_pagination"]["has_more"] is False
        assert len(connection.sends) == 1

    def test_small_batch_without_limit_is_returned_whole(self):
        connection = _CountingConnection(3)
        router = _core_router(connection)

        payload = json.loads(router("batch_query", {"commands": _batch_steps(3)}))

        assert [step["index"] for step in payload["results"]] == [0, 1, 2]
        assert "_pagination" not in payload
        assert "_note" not in payload
        assert len(connection.sends) == 1

    def test_oversized_batch_refusal_never_advises_rerunning(self):
        connection = _CountingConnection(30, blob=3_000)
        router = _core_router(connection)

        text = router("batch_query", {"commands": _batch_steps(30)})

        assert "split the batch" not in text
        assert "RESPONSE_TOO_LARGE" not in text

    def test_oversized_batch_pages_through_limit_without_loss(self):
        connection = _CountingConnection(30, blob=3_000)
        router = _core_router(connection)

        first = json.loads(router("batch_query", {"commands": _batch_steps(30), "limit": 20}))
        seen, _ = _drain_batch(router, first)

        assert seen == list(range(30))
        assert len(connection.sends) == 1

    def test_batch_cursor_cannot_be_used_on_another_read(self):
        connection = _CountingConnection(6)
        router = _core_router(connection)
        first = json.loads(router("batch_query", {"commands": _batch_steps(6), "limit": 2}))

        other = make_router("level", MagicMock(), "level docs")
        payload = json.loads(other("find_actors", {"cursor": first["_pagination"]["next_cursor"]}))

        assert payload["_error"] == "INVALID_CURSOR"


class TestSnapshotOwnedCursors:
    def test_pagination_meta_and_cursor_carry_snapshot_id(self):
        router, _ = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})

        first = json.loads(router("find_actors", {"limit": 2}))

        snapshot_id = first["_pagination"]["snapshot_id"]
        assert isinstance(snapshot_id, int) and snapshot_id >= 1
        cursor = first["_pagination"]["next_cursor"]
        assert json.loads(base64.b64decode(cursor))["snapshot_id"] == snapshot_id
        second = json.loads(router("find_actors", {"cursor": cursor}))
        assert second["_pagination"]["snapshot_id"] == snapshot_id

    def test_snapshot_ids_increase_monotonically(self):
        router, _ = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})

        ids = [json.loads(router("find_actors", {"limit": 2}))["_pagination"]["snapshot_id"] for _ in range(3)]

        assert ids == sorted(ids) and len(set(ids)) == 3

    def test_identical_limited_requests_keep_independent_cursors(self):
        connection = MagicMock()
        responses = [
            {"success": True, "data": {"matches": [{"name": f"A_{i}"} for i in range(6)], "count": 6}},
            {"success": True, "data": {"matches": [{"name": f"B_{i}"} for i in range(6)], "count": 6}},
        ]
        connection.send_command.side_effect = responses
        router = make_router("level", connection, "level docs")

        first = json.loads(router("find_actors", {"pattern": "X", "limit": 2}))
        second = json.loads(router("find_actors", {"pattern": "X", "limit": 2}))
        a_next = json.loads(router("find_actors", {"cursor": first["_pagination"]["next_cursor"]}))
        b_next = json.loads(router("find_actors", {"cursor": second["_pagination"]["next_cursor"]}))

        assert [m["name"] for m in a_next["matches"]] == ["A_2", "A_3"]
        assert [m["name"] for m in b_next["matches"]] == ["B_2", "B_3"]

    def test_tampered_snapshot_id_is_invalid_cursor(self):
        router, connection = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})
        cursor = json.loads(router("find_actors", {"limit": 2}))["_pagination"]["next_cursor"]
        connection.send_command.reset_mock()

        forged = _retag_cursor(cursor, snapshot_id=999)
        payload = json.loads(router("find_actors", {"cursor": forged}))

        assert payload["_error"] == "INVALID_CURSOR"
        connection.send_command.assert_not_called()

    def test_cursor_without_snapshot_id_is_invalid(self):
        router, _ = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})
        cursor = json.loads(router("find_actors", {"limit": 2}))["_pagination"]["next_cursor"]
        raw = json.loads(base64.b64decode(cursor))
        del raw["snapshot_id"]
        stripped = base64.b64encode(json.dumps(raw).encode()).decode()

        payload = json.loads(router("find_actors", {"cursor": stripped}))

        assert payload["_error"] == "INVALID_CURSOR"

    def test_evicted_snapshot_cursor_expires(self):
        router, connection = _router("level", {"matches": _actors(7), "count": 7, "pattern": "Actor"})
        cursor = json.loads(router("find_actors", {"pattern": "first", "limit": 2}))["_pagination"]["next_cursor"]
        for i in range(routers_mod._pagination_cache._max_entries):
            router("find_actors", {"pattern": f"other_{i}", "limit": 2})

        payload = json.loads(router("find_actors", {"cursor": cursor}))

        assert payload["_error"] == "CURSOR_EXPIRED"

    def test_router_cache_holds_sixteen_snapshots_for_ten_minutes(self):
        assert routers_mod._pagination_cache._max_entries == 16
        assert routers_mod._pagination_cache._ttl_seconds == 600


class TestNewReflectReadsCompleteOrRefuse:
    @pytest.mark.parametrize(
        ("command", "selector"),
        [
            ("find_duplicate_graphs", "path_prefixes"),
            ("find_name_lookups", "path_prefixes"),
            ("find_broken_config_paths", "row selector"),
        ],
    )
    def test_oversized_read_refuses_with_suggestion(self, command, selector):
        router, _ = _router("reflect", {"rows": [{"blob": "x" * 500} for _ in range(200)]})

        payload = json.loads(router(command, {"path_prefixes": ["/Game"]}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert payload["reader_complete"] is False
        assert selector in payload["_suggestion"]
        assert "rows" not in payload
        assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS

    @pytest.mark.parametrize("command", ["find_duplicate_graphs", "find_name_lookups"])
    def test_refusal_suggests_out_path_for_the_full_result(self, command):
        router, _ = _router("reflect", {"rows": [{"blob": "x" * 500} for _ in range(200)]})

        payload = json.loads(router(command, {"path_prefixes": ["/Game"]}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert "out_path" in payload["_suggestion"]
