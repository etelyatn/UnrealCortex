"""One public MCP failure shape (issue 226).

Every public tool failure is reported as ``{"success": false, "_error": CODE,
"_message": MESSAGE, ...details}`` and the public output adapter sets ``isError``. A raised
``UECommandError`` keeps the editor's code, message and non-reserved details; a raised
``ConnectionError`` becomes ``CONNECTION_ERROR``; non-JSON text stays opaque; other exceptions
still propagate. Domain tools that answered with another failure shape are converged here, one
representative per previous shape and per source family.
"""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import pytest
from mcp.server.fastmcp import FastMCP

from cortex_mcp.tcp_client import UECommandError

_FIXTURES = Path(__file__).resolve().parent / "fixtures"


def _run(coro_factory):
    asyncio.run(coro_factory())


def _text(result) -> str:
    blocks = [block for block in result.content if block.type == "text"]
    assert len(blocks) == 1, f"expected exactly one text block, got {result.content!r}"
    return blocks[0].text


def _payload(result) -> dict:
    payload = json.loads(_text(result))
    assert isinstance(payload, dict), f"expected a JSON object, got {payload!r}"
    return payload


def _registrar_server():
    """Real FastMCP behind the shipped output adapter, with no live connection."""
    from cortex_mcp.tool_output import ToolOutputRegistrar

    mcp = FastMCP("cortex-error-shape-contract")
    registrar = ToolOutputRegistrar(mcp, None)
    return mcp, registrar


def _assert_canonical_failure(result, *, code: str | None = None, message_contains: str | None = None) -> dict:
    assert result.isError is True, "a failure result must carry isError=True"
    payload = _payload(result)
    assert payload["success"] is False, f"canonical failures carry success=false: {payload!r}"
    assert isinstance(payload.get("_error"), str) and payload["_error"], (
        f"canonical failures carry a non-empty _error code: {payload!r}"
    )
    assert isinstance(payload.get("_message"), str) and payload["_message"], (
        f"canonical failures carry a non-empty _message: {payload!r}"
    )
    if code is not None:
        assert payload["_error"] == code
    if message_contains is not None:
        assert message_contains in payload["_message"], (
            f"expected {message_contains!r} in {payload['_message']!r}"
        )
    return payload


class _FakeConnection:
    """Minimal connection double: scripted responses, optional scripted failure, no network."""

    def __init__(self, *, error: BaseException | None = None, responses: list | None = None):
        self.error = error
        self.responses = list(responses or [])
        self.calls: list[tuple[str, object]] = []

    def _respond(self, command, params):
        self.calls.append((command, params))
        if self.error is not None:
            raise self.error
        if not self.responses:
            return {"success": True, "data": {}}
        return self.responses.pop(0)

    def send_command(self, command, params=None, **kwargs):
        return self._respond(command, params)

    def send_command_cached(self, command, params=None, **kwargs):
        return self._respond(command, params)

    def send_command_once(self, command, params=None, **kwargs):
        return self._respond(command, params)

    def record_tool_invocation(self, *args, **kwargs):
        return None

    def invalidate_cache(self, *args, **kwargs):
        return None


# --------------------------------------------------------------------------------------
# The public output adapter owns isError and the error conversion
# --------------------------------------------------------------------------------------

def test_registrar_marks_a_non_empty_error_code_as_is_error():
    async def scenario():
        mcp, registrar = _registrar_server()
        canonical = {"success": False, "_error": "INVALID_FIELD", "_message": "value is required"}

        @registrar.tool(name="probe_dict_failure", description="probe dict failure")
        def probe_dict_failure() -> dict:
            return canonical

        @registrar.tool(name="probe_text_failure", description="probe text failure")
        def probe_text_failure() -> str:
            return json.dumps(canonical)

        for name in ("probe_dict_failure", "probe_text_failure"):
            result = await mcp.call_tool(name, {})
            assert result.isError is True, name
            assert _payload(result) == canonical, name

    _run(scenario)


def test_registrar_marks_only_a_non_empty_error_code_as_is_error():
    async def scenario():
        mcp, registrar = _registrar_server()
        outcomes = {
            "probe_success": {"success": True, "data": {"saved": True}},
            # success=false without a code is a reported outcome, not a decoded failure code.
            "probe_codeless_outcome": {"success": False, "data": {"status": "cancelled"}},
            # An empty code is not a code.
            "probe_empty_code": {"success": False, "_error": "", "_message": "no code"},
        }

        def _constant_tool(payload):
            def probe() -> dict:
                return payload

            return probe

        for name, payload in outcomes.items():
            registrar.tool(name=name, description=f"probe {name}")(_constant_tool(payload))

        for name, payload in outcomes.items():
            result = await mcp.call_tool(name, {})
            assert result.isError is False, name
            assert _payload(result) == payload, name

    _run(scenario)


def test_registrar_converts_a_raised_ue_command_error_into_the_canonical_shape():
    async def scenario():
        mcp, registrar = _registrar_server()

        @registrar.tool(name="probe_raises_ue_command_error", description="probe editor failure")
        def probe_raises_ue_command_error() -> str:
            raise UECommandError(
                "level.set_transform",
                "INVALID_VALUE",
                "location must be a 3-number array",
                {"location": [1, 2], "success": True, "_message": "spoofed"},
            )

        result = await mcp.call_tool("probe_raises_ue_command_error", {})
        payload = _assert_canonical_failure(
            result, code="INVALID_VALUE", message_contains="3-number array"
        )
        assert payload["_message"] == "location must be a 3-number array"
        # Non-reserved details survive; reserved envelope keys never overwrite the editor's.
        assert payload["location"] == [1, 2]

    _run(scenario)


def test_registrar_converts_errors_raised_by_an_async_tool():
    async def scenario():
        mcp, registrar = _registrar_server()

        @registrar.tool(name="probe_async_ue_error", description="probe async editor failure")
        async def probe_async_ue_error() -> str:
            await asyncio.sleep(0)
            raise UECommandError(
                "gen.start_mesh", "PROVIDER_NOT_FOUND", "No provider configured"
            )

        result = await mcp.call_tool("probe_async_ue_error", {})
        _assert_canonical_failure(
            result, code="PROVIDER_NOT_FOUND", message_contains="No provider configured"
        )

    _run(scenario)


def test_registrar_converts_a_raised_connection_error_into_connection_error():
    async def scenario():
        mcp, registrar = _registrar_server()

        @registrar.tool(name="probe_raises_connection_error", description="probe connection failure")
        def probe_raises_connection_error() -> str:
            raise ConnectionError("editor not reachable on port 8742")

        result = await mcp.call_tool("probe_raises_connection_error", {})
        _assert_canonical_failure(
            result, code="CONNECTION_ERROR", message_contains="editor not reachable"
        )

    _run(scenario)


def test_registrar_propagates_programming_errors_instead_of_reencoding_them():
    async def scenario():
        mcp, registrar = _registrar_server()

        @registrar.tool(name="probe_programming_error", description="probe programmer mistake")
        def probe_programming_error() -> str:
            raise ValueError("unhandled bug in the tool body")

        with pytest.raises(Exception) as excinfo:
            await mcp.call_tool("probe_programming_error", {})
        assert "unhandled bug in the tool body" in str(excinfo.value)

    _run(scenario)


def test_registrar_leaves_non_json_failure_text_opaque():
    async def scenario():
        mcp, registrar = _registrar_server()
        opaque = "Error: editor not reachable\n  CORTEX_PROJECT_DIR: D:/x"

        @registrar.tool(name="probe_opaque_failure", description="probe opaque failure")
        def probe_opaque_failure() -> str:
            return opaque

        result = await mcp.call_tool("probe_opaque_failure", {})
        # Non-JSON text is never parsed for a code, so it is not claimed as a decoded failure.
        assert result.isError is False
        assert _text(result) == opaque

    _run(scenario)


# --------------------------------------------------------------------------------------
# Reflect standalone tools (previously: UECommandError raised, ConnectionError as text)
# --------------------------------------------------------------------------------------

def test_query_class_detail_surfaces_the_editor_suggestions_as_a_canonical_error():
    async def scenario():
        from tools.reflect.detail import register_reflect_detail_tools

        connection = _FakeConnection(
            error=UECommandError(
                "reflect.class_detail",
                "CLASS_NOT_FOUND",
                "No class named 'Nope' in the project or engine",
                {"suggestions": ["APlayerController", "AController"]},
            )
        )
        mcp, registrar = _registrar_server()
        register_reflect_detail_tools(registrar, connection)

        result = await mcp.call_tool("query_class_detail", {"class_name": "Nope"})
        payload = _assert_canonical_failure(
            result, code="CLASS_NOT_FOUND", message_contains="Nope"
        )
        assert payload["suggestions"] == ["APlayerController", "AController"]

    _run(scenario)


def test_query_class_detail_connection_failure_is_a_canonical_connection_error():
    async def scenario():
        from tools.reflect.detail import register_reflect_detail_tools

        connection = _FakeConnection(error=ConnectionError("editor not reachable"))
        mcp, registrar = _registrar_server()
        register_reflect_detail_tools(registrar, connection)

        result = await mcp.call_tool("query_class_detail", {"class_name": "AActor"})
        _assert_canonical_failure(result, code="CONNECTION_ERROR")

    _run(scenario)


def test_query_class_detail_success_is_not_an_error_result():
    async def scenario():
        from tools.reflect.detail import register_reflect_detail_tools

        connection = _FakeConnection(
            responses=[
                {"success": True, "data": {"name": "AActor", "type": "class", "parent": "UObject"}}
            ]
        )
        mcp, registrar = _registrar_server()
        register_reflect_detail_tools(registrar, connection)

        result = await mcp.call_tool("query_class_detail", {"class_name": "AActor"})
        assert result.isError is False
        assert _payload(result)["name"] == "AActor"

    _run(scenario)


# --------------------------------------------------------------------------------------
# Domain routers (previously: "Error: ..." text; "_switch_editor" used {error} / {error,message})
# --------------------------------------------------------------------------------------

def test_router_connection_failure_is_a_canonical_connection_error():
    async def scenario():
        from cortex_mcp.tools.routers import make_router

        connection = _FakeConnection(error=ConnectionError("connection refused"))
        mcp, registrar = _registrar_server()
        registrar.tool(name="level_cmd")(make_router("level", connection, "level docs"))

        result = await mcp.call_tool("level_cmd", {"command": "list_actors", "params": {}})
        _assert_canonical_failure(
            result, code="CONNECTION_ERROR", message_contains="connection refused"
        )

    _run(scenario)


def test_switch_editor_dead_pid_is_a_canonical_editor_not_found():
    async def scenario():
        from cortex_mcp.tools.routers import make_router

        connection = _FakeConnection()
        mcp, registrar = _registrar_server()
        registrar.tool(name="core_cmd")(make_router("core", connection, "core docs"))
        editor = SimpleNamespace(pid=1000, port=8742, started_at="2026-01-01T00:00:00Z", project="")

        with patch("cortex_mcp.tools.routers._discover_all_editors", return_value=[editor]), patch(
            "cortex_mcp.tools.routers._is_editor_alive", return_value=False
        ):
            result = await mcp.call_tool(
                "core_cmd", {"command": "switch_editor", "params": {"pid": 9999}}
            )

        _assert_canonical_failure(result, code="EDITOR_NOT_FOUND", message_contains="9999")

    _run(scenario)


# --------------------------------------------------------------------------------------
# Composite tools (previously: {success: false, error}, codeless failures, "Error: ..." text)
# --------------------------------------------------------------------------------------

def test_level_compose_invalid_spec_is_a_canonical_failure():
    async def scenario():
        from cortex_mcp.tools.composites.level import register_level_compose_tools

        connection = _FakeConnection()
        mcp, registrar = _registrar_server()
        register_level_compose_tools(registrar, connection)

        result = await mcp.call_tool("level_compose", {"operations": [{"op": "spawn"}]})
        _assert_canonical_failure(result, message_contains="Invalid spec")
        assert connection.calls == []

    _run(scenario)


def test_level_compose_failed_step_carries_the_native_error_code():
    async def scenario():
        from cortex_mcp.tools.composites.level import register_level_compose_tools

        failed_batch = {
            "success": True,
            "data": {
                "results": [
                    {
                        "index": 0,
                        "success": False,
                        "command": "level.modify_actor",
                        "error_code": "ACTOR_NOT_FOUND",
                        "error_message": "Actor 'OldLight' was not found",
                    }
                ],
                "total_timing_ms": 1,
            },
        }
        connection = _FakeConnection(responses=[failed_batch])
        mcp, registrar = _registrar_server()
        register_level_compose_tools(registrar, connection)

        result = await mcp.call_tool(
            "level_compose",
            {"operations": [{"op": "modify", "actor": "OldLight", "folder": "Lighting/Deprecated"}]},
        )
        payload = _assert_canonical_failure(result, code="ACTOR_NOT_FOUND")
        # The per-step diagnostics stay available as details.
        assert payload["failed_steps"][0]["error_code"] == "ACTOR_NOT_FOUND"

    _run(scenario)


def test_gen_compose_unknown_type_is_a_canonical_failure():
    async def scenario():
        from cortex_mcp.tools.composites.gen import register_gen_compose_tools

        connection = _FakeConnection()
        mcp, registrar = _registrar_server()
        register_gen_compose_tools(registrar, connection)

        result = await mcp.call_tool("gen_compose", {"type": "unsupported_modality"})
        _assert_canonical_failure(result, message_contains="unsupported_modality")
        assert connection.calls == []

    _run(scenario)


def test_editor_restart_without_a_project_is_a_canonical_failure(monkeypatch):
    from tools.editor import composites as editor_composites
    from cortex_mcp.tools.standalone.editor import register_editor_standalone_tools

    async def scenario():
        connection = _FakeConnection()
        mcp, registrar = _registrar_server()
        monkeypatch.setattr(editor_composites, "resolve_project_dir", lambda: None)
        register_editor_standalone_tools(registrar, connection)

        result = await mcp.call_tool("editor_restart", {})
        _assert_canonical_failure(result, message_contains="project directory")
        assert connection.calls == []

    _run(scenario)


def test_qa_test_step_connection_failure_is_a_canonical_connection_error():
    async def scenario():
        from cortex_mcp.tools.standalone.qa import register_qa_standalone_tools

        connection = _FakeConnection(error=ConnectionError("editor not reachable"))
        mcp, registrar = _registrar_server()
        register_qa_standalone_tools(registrar, connection)

        result = await mcp.call_tool(
            "qa_test_step", {"action": {"command": "qa.observe_state", "params": {}}}
        )
        _assert_canonical_failure(result, code="CONNECTION_ERROR")

    _run(scenario)


def test_apply_import_ops_connection_failure_is_a_canonical_connection_error():
    async def scenario():
        from tools.data.import_queues import register_import_queue_tools

        connection = _FakeConnection(error=ConnectionError("editor not reachable"))
        mcp, registrar = _registrar_server()
        register_import_queue_tools(registrar, connection)

        result = await mcp.call_tool(
            "apply_import_ops_json", {"ops_path": "ops.json", "report_path": "report.json"}
        )
        _assert_canonical_failure(result, code="CONNECTION_ERROR")

    _run(scenario)


# --------------------------------------------------------------------------------------
# Declared parameter surface: editor.start_pie declares only what it reads
# --------------------------------------------------------------------------------------

def test_editor_start_pie_schema_declares_only_mode():
    from cortex_mcp._fallback_generated import FALLBACK_COMMANDS

    fallback = {command["name"]: command for command in FALLBACK_COMMANDS["editor"]}
    assert fallback["start_pie"]["params"] == [
        {"name": "mode", "required": False, "type": "string"}
    ]

    fixture = json.loads((_FIXTURES / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    published = {
        command["name"]: command for command in fixture["domains"]["editor"]["commands"]
    }
    assert [param["name"] for param in published["start_pie"]["params"]] == ["mode"]


# --------------------------------------------------------------------------------------
# Envelope warnings are part of the public result
# --------------------------------------------------------------------------------------

def _call_router(domain: str, command: str, params: dict | None, response: dict):
    """Call one registered router tool whose scripted native envelope is ``response``."""
    from cortex_mcp.tools.routers import make_router

    connection = _FakeConnection(responses=[response])
    mcp, registrar = _registrar_server()
    registrar.tool(name=f"{domain}_cmd")(make_router(domain, connection, f"{domain} docs"))
    return asyncio.run(
        mcp.call_tool(f"{domain}_cmd", {"command": command, "params": params or {}})
    )


def test_router_forwards_native_envelope_warnings():
    # A write that applied but could not use every member reports warnings beside `data`; the
    # caller must receive them, or the honest write is invisible in the public result.
    warnings = ["property 'Health' could not be read and was left unchanged"]
    result = _call_router(
        "data",
        "get_warning_probe",
        {},
        {"success": True, "data": {"value": 5}, "warnings": warnings},
    )

    assert result.isError is False
    payload = _payload(result)
    assert payload["value"] == 5
    assert payload["warnings"] == warnings


def test_router_keeps_data_level_warnings_over_envelope_warnings():
    result = _call_router(
        "data",
        "get_warning_probe",
        {},
        {"success": True, "data": {"warnings": ["data-level"]}, "warnings": ["envelope"]},
    )

    assert _payload(result)["warnings"] == ["data-level"]


def test_router_does_not_add_empty_envelope_warnings():
    result = _call_router(
        "data", "get_warning_probe", {}, {"success": True, "data": {"ok": True}, "warnings": []}
    )

    assert "warnings" not in _payload(result)


def test_router_forwards_warnings_from_a_native_page_read():
    result = _call_router(
        "level",
        "list_actors",
        {},
        {"success": True, "data": {"actors": []}, "warnings": ["clamped to the page"]},
    )

    assert _payload(result)["warnings"] == ["clamped to the page"]


def test_router_forwards_warnings_from_a_cached_read():
    result = _call_router(
        "level",
        "list_actor_classes",
        {},
        {"success": True, "data": {"classes": []}, "warnings": ["cached warning"]},
    )

    assert _payload(result)["warnings"] == ["cached warning"]


def test_batch_query_forwards_envelope_warnings():
    result = _call_router(
        "core",
        "batch_query",
        {"commands": [{"command": "probe"}]},
        {
            "success": True,
            "data": {"results": [{"index": 0, "success": True}], "total_timing_ms": 1},
            "warnings": ["batch warning"],
        },
    )

    assert _payload(result)["warnings"] == ["batch warning"]


# --------------------------------------------------------------------------------------
# Oversize refusals are failures with a message
# --------------------------------------------------------------------------------------

def test_oversized_response_without_an_array_refusal_has_a_message():
    huge = "x" * 41000
    result = _call_router(
        "data", "get_oversize_probe", {}, {"success": True, "data": {"blob": huge}}
    )

    payload = _assert_canonical_failure(result, code="RESPONSE_TOO_LARGE")
    assert isinstance(payload["_size"], int) and payload["_size"] > 40000
    assert "limit" in payload["_suggestion"]


def test_oversized_single_array_entry_refusal_has_a_message():
    huge = "y" * 41000
    data = {"items": [huge] + ["y"] * 10}
    result = _call_router(
        "data", "get_oversize_probe", {}, {"success": True, "data": data}
    )

    payload = _assert_canonical_failure(result, code="RESPONSE_TOO_LARGE")
    assert isinstance(payload["_size"], int) and payload["_size"] > 40000
    assert "items" in payload["_suggestion"]
