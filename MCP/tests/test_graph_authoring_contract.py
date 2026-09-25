"""Live-contract tests for the published `graph.apply_patch` authoring envelope.

The native registration in `FCortexGraphCommandHandler::GetSupportedCommands()` is the single
owner of the published schema, and `MCP/tests/fixtures/capabilities_cache_full.json` mirrors that
payload. These tests pin the *published contract* the MCP surface exposes: envelope fields,
limits, family identifiers, the standalone-versus-batch restriction and the `UMGAuthoring`
policy boundary. The corresponding drift check between the native description and the live
`graph.get_authoring_context` limits runs natively, where both sources exist.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from unittest.mock import MagicMock

import pytest

from cortex_mcp.operation_schema import build_profile_operation_schema, reset_operation_schema_budget
from cortex_mcp._fallback_generated import FALLBACK_COMMANDS
from cortex_mcp.tools.routers import make_router

FIXTURE = Path(__file__).parent / "fixtures" / "capabilities_cache_full.json"

ENVELOPE_REQUIRED = ["asset_path", "target", "patch_id", "expected_fingerprint", "nodes", "connections"]
ENVELOPE_OPTIONAL = [
    "pin_updates",
    "dry_run",
    "compile",
    "save",
    "allow_noop",
    "expected_validation_hash",
]

# The native authoring contract's defaults, as published in the parameter descriptions.
ENVELOPE_DEFAULTS = {
    "dry_run": "true",
    "compile": "true",
    "save": "false",
    "allow_noop": "false",
}

# `graph.get_authoring_context` publishes these live values; the patch schema must not drift.
NATIVE_LIMITS = {
    "max_nodes": 64,
    "max_edges": 256,
    "max_client_id_length": 32,
    "max_request_size_bytes": 65536,
    "max_scanned_nodes": 2048,
}

# The native authoring families published by `graph.get_authoring_context`.
NATIVE_FAMILIES = [
    "CallFunction",
    "VariableGet",
    "VariableSet",
    "Self",
    "DynamicCast",
    "ConstructObject",
    "Event",
]


def _capabilities() -> dict:
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


def _graph_command(name: str) -> dict:
    commands = _capabilities()["domains"]["graph"]["commands"]
    for command in commands:
        if command["name"] == name:
            return command
    raise AssertionError(f"graph.{name} is missing from the capabilities fixture")


def _param(command: dict, name: str) -> dict:
    for param in command["params"]:
        if param["name"] == name:
            return param
    raise AssertionError(f"parameter {name} is missing from the published schema")


def _live_schema_connection(command: dict) -> MagicMock:
    connection = MagicMock()
    connection.send_command.return_value = {
        "success": True,
        "data": {
            "source": "live_editor",
            "editor_instance_id": "instance-1",
            "plugin_build_id": "0.3.0-24601",
            "domain": "graph",
            "command": command["name"],
            "router": "graph_cmd",
            "params": command["params"],
        },
    }
    return connection


def test_apply_patch_is_published_with_the_exact_envelope_fields():
    command = _graph_command("apply_patch")
    required = [param["name"] for param in command["params"] if param["required"]]
    optional = [param["name"] for param in command["params"] if not param["required"]]
    assert required == ENVELOPE_REQUIRED
    assert optional == ENVELOPE_OPTIONAL
    for param in command["params"]:
        assert param["description"], param["name"]


def test_published_defaults_and_limits_match_the_native_authoring_contract():
    command = _graph_command("apply_patch")
    description = command["description"]
    for flag, default in ENVELOPE_DEFAULTS.items():
        assert f"default: {default}" in _param(command, flag)["description"], flag

    published = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", description)}
    for key, value in NATIVE_LIMITS.items():
        assert published.get(key) == value, f"{key} drifted from the native limit"


def test_published_families_match_the_native_authoring_family_list():
    description = _param(_graph_command("apply_patch"), "nodes")["description"]
    match = re.search(r"Supported families: ([^.]+)", description)
    assert match, description
    published = [family.strip() for family in match.group(1).split(",")]
    assert published == NATIVE_FAMILIES


def test_published_schema_declares_the_standalone_batch_restriction():
    description = _graph_command("apply_patch")["description"]
    assert "rollback-enabled" in description
    assert "batch" in description


def test_published_apply_patch_description_documents_entry_retirement():
    command = _graph_command("apply_patch")
    assert "retire_entries" in command["description"]
    assert "entry_node_guids" in command["description"]
    assert [param["name"] for param in command["params"]] == (
        ENVELOPE_REQUIRED + ENVELOPE_OPTIONAL
    )


def test_umg_authoring_profile_exposes_apply_patch_through_graph_cmd():
    reset_operation_schema_budget()
    connection = _live_schema_connection(_graph_command("apply_patch"))
    native_schema = connection.send_command.return_value["data"]
    native_snapshot = json.loads(json.dumps(native_schema))
    payload = json.loads(
        build_profile_operation_schema(connection, "UMGAuthoring", "graph", "apply_patch")
    )
    assert payload["source"] == "live_editor"
    assert payload["editor_available"] is True
    assert payload["policy_allowed"] is True
    assert payload["execution_shape"] == {"type": "router", "tool": "graph_cmd"}
    assert [param["name"] for param in payload["params"]][0] == "asset_path"
    assert payload["mcp_limits"]["max_response_chars"] == 40_000
    assert all(param["name"] != "max_response_chars" for param in payload["params"])
    assert native_schema == native_snapshot
    assert "max_response_chars" not in native_schema
    assert NATIVE_LIMITS["max_scanned_nodes"] == 2048
    assert payload["mcp_limits"]["max_response_chars"] != NATIVE_LIMITS["max_scanned_nodes"]


def test_umg_authoring_profile_still_blocks_unrelated_blueprint_commands():
    reset_operation_schema_budget()
    connection = MagicMock()
    connection.send_command.return_value = {
        "success": True,
        "data": {"source": "live_editor", "params": [{"name": "asset_path", "type": "string", "required": True}]},
    }
    payload = json.loads(
        build_profile_operation_schema(connection, "UMGAuthoring", "blueprint", "add_variable")
    )
    assert payload["editor_available"] is True
    assert payload["policy_allowed"] is False
    assert "blueprint.add_variable" in payload["blocked_reason"]


def test_graph_cmd_qualifies_and_forwards_non_prune_apply_patch_once():
    connection = MagicMock()
    connection.send_command.return_value = {
        "success": True,
        "data": {"patch_id": "8190c0d0-b23b-4de8-a8a8-4625a35b4a52", "changed": False},
    }
    router = make_router("graph", connection, "graph docs")
    params = {"asset_path": "/Game/Temp/BP_Test.BP_Test", "patch_id": "8190c0d0-b23b-4de8-a8a8-4625a35b4a52"}
    payload = json.loads(router("apply_patch", params))
    connection.send_command.assert_called_once_with("graph.apply_patch", params)
    assert payload["patch_id"] == "8190c0d0-b23b-4de8-a8a8-4625a35b4a52"
    assert payload["changed"] is False


def test_graph_cmd_surfaces_apply_patch_errors_without_a_batch_fallback():
    from cortex_mcp.tcp_client import UECommandError

    connection = MagicMock()
    connection.send_command.side_effect = UECommandError(
        "graph.apply_patch", "STALE_PRECONDITION", "graph_authoring_hash mismatch", {},
    )
    router = make_router("graph", connection, "graph docs")
    payload = json.loads(router("apply_patch", {"asset_path": "/Game/Temp/BP_Test.BP_Test"}))
    assert payload["success"] is False
    assert payload["_error"] == "STALE_PRECONDITION"
    assert [call.args[0] for call in connection.send_command.call_args_list] == ["graph.apply_patch"]


def test_fallback_registration_carries_apply_patch():
    """`_fallback_generated.py` is regenerated from the fixture, so it must agree with it."""
    fallback = {command["name"]: command for command in FALLBACK_COMMANDS["graph"]}
    assert "apply_patch" in fallback
    published = _graph_command("apply_patch")
    assert fallback["apply_patch"]["params"] == [
        {"name": param["name"], "required": param["required"], "type": param["type"]}
        for param in published["params"]
    ]


def test_fixture_publishes_the_whole_implemented_graph_registration():
    names = [command["name"] for command in _capabilities()["domains"]["graph"]["commands"]]
    assert "get_authoring_context" in names
    assert "apply_patch" in names
    assert len(names) == len(set(names))


@pytest.mark.parametrize("field", ENVELOPE_REQUIRED)
def test_every_required_envelope_field_is_documented_with_a_type(field):
    param = _param(_graph_command("apply_patch"), field)
    assert param["type"] in {"string", "object", "array"}
