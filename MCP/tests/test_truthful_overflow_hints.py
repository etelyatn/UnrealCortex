"""Truthful overflow hints (issue #225).

The generic array truncator used to always suggest passing ``limit``. For reads that do
not page, that parameter is refused natively and the suggestion costs the model a failed
repeat call. The suggestion must name the narrowing the read actually supports, and only
reads whose owner pages may advertise ``limit``.
"""

import json

from cortex_mcp.response import MAX_RESPONSE_CHARS, format_response


def _oversized_blueprint_read():
    return {
        "asset_path": "/Game/BP_X",
        "functions": [
            {"name": f"Func_{i}", "inputs": [{"name": "A", "type": "float"},
                                             {"name": "B", "type": "float"},
                                             {"name": "C", "type": "float"}]}
            for i in range(600)
        ],
    }


class TestTruncationHintTruthfulness:
    def test_non_paging_read_truncation_does_not_advertise_limit(self):
        result = json.loads(
            format_response(_oversized_blueprint_read(), "blueprint_cmd", command="get_info")
        )

        assert result["_truncated"]["original_count"] > result["_truncated"]["returned_count"]
        suggestion = result["_truncated"]["suggestion"].lower()
        assert "limit" not in suggestion
        assert "narrow" in suggestion or "scope" in suggestion or "fewer" in suggestion

    def test_non_paging_refusal_does_not_advertise_limit(self):
        data = {"details": "x" * (MAX_RESPONSE_CHARS + 1000), "items": []}

        result = json.loads(
            format_response(data, "blueprint_cmd", command="get_info")
        )

        assert result["_error"] == "RESPONSE_TOO_LARGE"
        suggestion = result["_suggestion"].lower()
        assert "limit" not in suggestion
        assert "narrow" in suggestion or "scope" in suggestion or "fewer" in suggestion

    def test_paging_read_truncation_keeps_the_limit_suggestion(self):
        result = json.loads(
            format_response(_oversized_blueprint_read(), "blueprint_cmd", command="list", pages=True)
        )

        assert result["_truncated"]["original_count"] > result["_truncated"]["returned_count"]
        assert "limit" in result["_truncated"]["suggestion"].lower()

    def test_paging_read_oversized_single_entry_still_suggests_narrowing(self):
        data = {
            "blueprints": [
                {"name": f"B{i}", "payload": "x" * (MAX_RESPONSE_CHARS + 100)}
                for i in range(12)
            ],
        }

        result = json.loads(
            format_response(data, "blueprint_cmd", command="list", pages=True)
        )

        assert result["_error"] == "RESPONSE_TOO_LARGE"
        assert "smaller scope" in result["_suggestion"].lower() or "narrow" in result["_suggestion"].lower()


class TestSelectorPolicyEntries:
    """Traces and the flat hierarchy read refuse naming their own narrowing selectors."""

    def test_trace_exec_oversize_refuses_naming_max_depth(self):
        data = {
            "asset_path": "/Game/BP_X",
            "nodes": [{"name": f"N{i}", "payload": "x" * 300} for i in range(400)],
        }

        result = json.loads(
            format_response(data, "graph_cmd", qualified_command="graph.trace_exec")
        )

        assert result["_error"] == "RESPONSE_TOO_LARGE"
        assert result["reader_complete"] is False
        assert "max_depth" in result["_suggestion"]

    def test_trace_dataflow_oversize_refuses_naming_max_depth(self):
        data = {
            "asset_path": "/Game/BP_X",
            "nodes": [{"name": f"N{i}", "payload": "x" * 300} for i in range(400)],
        }

        result = json.loads(
            format_response(data, "graph_cmd", qualified_command="graph.trace_dataflow")
        )

        assert result["_error"] == "RESPONSE_TOO_LARGE"
        assert "max_depth" in result["_suggestion"]

    def test_hierarchy_oversize_refuses_naming_max_results(self):
        data = {
            "name": "AActor",
            "children": [{"name": f"C{i}", "payload": "x" * 300} for i in range(400)],
            "total_classes": 400,
        }

        # The routed form: reflect_cmd passes qualified_command="reflect.class_hierarchy".
        result = json.loads(
            format_response(data, "reflect_cmd", qualified_command="reflect.class_hierarchy")
        )

        assert result["_error"] == "RESPONSE_TOO_LARGE"
        assert result["reader_complete"] is False
        assert "max_results" in result["_suggestion"]

    def test_routed_hierarchy_overflow_refuses_through_the_router(self):
        from unittest.mock import MagicMock
        from cortex_mcp.tools.routers import make_router

        data = {
            "name": "AActor",
            "children": [{"name": f"C{i}", "payload": "x" * 300} for i in range(400)],
            "total_classes": 400,
        }
        connection = MagicMock()
        connection.send_command.return_value = {"success": True, "data": data}
        router = make_router("reflect", connection, "reflect docs")

        payload = json.loads(router("class_hierarchy", {"root": "AActor"}))

        assert payload["_error"] == "RESPONSE_TOO_LARGE"
        assert "max_results" in payload["_suggestion"]
