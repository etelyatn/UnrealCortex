"""Transport, shape and refusal tests for the safe Blueprint update route.

`blueprint_compose(mode="update")` must forward exactly one reviewed `graph.apply_patch`
envelope and must never reach the removed legacy batch update route. Native validation stays
authoritative in C++: these tests only pin the facade's transport and shape contract.
"""

from __future__ import annotations

import json
from unittest.mock import MagicMock

from cortex_mcp.response import MAX_RESPONSE_CHARS
from cortex_mcp.tcp_client import UECommandError
from cortex_mcp.tools.composites.blueprint import register_blueprint_compose_tools


class Capture:
    """Minimal decorator-capture MCP shim matching the FastMCP registration contract."""

    def __init__(self):
        self.tools = {}

    def tool(self, name=None, description=None, **_kwargs):
        def decorator(fn):
            self.tools[name or fn.__name__] = fn
            return fn

        return decorator


ASSET = "/Game/Temp/BP_Test.BP_Test"
PATCH_ID = "8190c0d0-b23b-4de8-a8a8-4625a35b4a52"


def _register(connection):
    """Register the facade against a mock connection and return (mcp, tool, connection)."""
    mcp = Capture()
    register_blueprint_compose_tools(mcp, connection)
    return mcp, mcp.tools["blueprint_compose"], connection


def _payload(raw: str) -> dict:
    return json.loads(raw)


def _native_data(**overrides) -> dict:
    data = {
        "patch_id": PATCH_ID,
        "changed": False,
        "dry_run": True,
        "apply_status": "not_requested",
        "compile_status": "not_requested",
        "readback_status": "not_requested",
        "rollback_status": "not_requested",
        "save_status": "not_requested",
        "post_save_status": "not_requested",
    }
    data.update(overrides)
    return data


def _forwarded_request(connection, **call_kwargs) -> dict:
    assert connection.send_command.call_count == 1
    args, _kwargs = connection.send_command.call_args
    assert args[0] == "graph.apply_patch"
    return args[1]


def test_update_forwards_one_native_patch_without_legacy_batch():
    mcp, connection = Capture(), MagicMock()
    connection.send_command.return_value = {"success": True, "data": {"changed": False}}
    register_blueprint_compose_tools(mcp, connection)
    patch = {"patch_id": "8190c0d0-b23b-4de8-a8a8-4625a35b4a52", "dry_run": True}
    mcp.tools["blueprint_compose"](
        mode="update", asset_path="/Game/Temp/BP_Test.BP_Test", patch=patch)
    connection.send_command.assert_called_once()
    args, _kwargs = connection.send_command.call_args
    assert args[0] == "graph.apply_patch"
    assert args[1] == {"asset_path": "/Game/Temp/BP_Test.BP_Test", **patch}


def test_update_default_legacy_kwargs_are_not_treated_as_mixed():
    """Facade defaults (type/graph_name) are declared values, not supplied legacy fields."""
    mcp, tool, connection = _register(MagicMock())
    connection.send_command.return_value = {"success": True, "data": _native_data()}
    tool(
        mode="update",
        asset_path=ASSET,
        type="Actor",
        graph_name="EventGraph",
        variables=None,
        functions=None,
        nodes=None,
        connections=None,
        subgraph_path="",
        graph_kind="",
        owning_interface="",
        name="",
        path="",
        parent_class="",
        patch={"patch_id": PATCH_ID, "dry_run": True},
    )
    request = _forwarded_request(connection)
    assert request == {"asset_path": ASSET, "patch_id": PATCH_ID, "dry_run": True}


def test_update_empty_legacy_collections_are_not_mixed_fields():
    """An empty list is not a legacy payload; only real legacy content conflicts with patch."""
    mcp, tool, connection = _register(MagicMock())
    connection.send_command.return_value = {"success": True, "data": _native_data()}
    tool(
        mode="update",
        asset_path=ASSET,
        variables=[],
        functions=[],
        nodes=[],
        connections=[],
        patch={"patch_id": PATCH_ID, "dry_run": True},
    )
    request = _forwarded_request(connection)
    assert request["asset_path"] == ASSET
    assert request["patch_id"] == PATCH_ID


def test_update_without_patch_reports_migration_error_and_never_calls_editor():
    mcp, tool, connection = _register(MagicMock())
    payload = _payload(tool(
        mode="update", asset_path=ASSET, nodes=[{"name": "N", "class": "CallFunction"}],
    ))
    assert payload["success"] is False
    assert payload["_error"] == "MIGRATION_REQUIRED"
    assert "graph.apply_patch" in payload["_message"]
    assert "patch" in payload["_message"]
    connection.send_command.assert_not_called()


def test_update_mixed_legacy_fields_with_patch_fail_locally():
    mcp, tool, connection = _register(MagicMock())
    payload = _payload(tool(
        mode="update",
        asset_path=ASSET,
        nodes=[{"name": "N", "class": "CallFunction"}],
        patch={"patch_id": PATCH_ID, "dry_run": True},
    ))
    assert payload["success"] is False
    assert payload["_error"] == "MIXED_UPDATE_CONTRACT"
    assert "nodes" in payload["_message"]
    connection.send_command.assert_not_called()


def test_update_legacy_fingerprint_guard_must_move_inside_patch():
    mcp, tool, connection = _register(MagicMock())
    payload = _payload(tool(
        mode="update",
        asset_path=ASSET,
        expected_fingerprint={"graph_authoring_hash": "abc"},
        patch={"patch_id": PATCH_ID, "dry_run": True},
    ))
    assert payload["success"] is False
    assert payload["_error"] == "MIXED_UPDATE_CONTRACT"
    assert "patch.expected_fingerprint" in payload["_message"]
    connection.send_command.assert_not_called()


def test_update_patch_must_be_an_object():
    mcp, tool, connection = _register(MagicMock())
    payload = _payload(tool(
        mode="update", asset_path=ASSET, patch=["not", "an", "object"],
    ))
    assert payload["success"] is False
    assert payload["_error"] == "INVALID_PATCH"
    assert "object" in payload["_message"]
    connection.send_command.assert_not_called()


def test_update_patch_must_not_be_empty():
    mcp, tool, connection = _register(MagicMock())
    payload = _payload(tool(mode="update", asset_path=ASSET, patch={}))
    assert payload["success"] is False
    assert payload["_error"] == "INVALID_PATCH"
    connection.send_command.assert_not_called()


def test_update_requires_asset_path():
    mcp, tool, connection = _register(MagicMock())
    payload = _payload(tool(
        mode="update", asset_path="", patch={"patch_id": PATCH_ID, "dry_run": True},
    ))
    assert payload["success"] is False
    assert payload["_error"] == "INVALID_PATCH"
    assert "asset_path" in payload["_message"]
    connection.send_command.assert_not_called()


def test_update_rejects_non_boolean_patch_flags_locally():
    for flag in ("dry_run", "compile", "save", "allow_noop"):
        mcp, tool, connection = _register(MagicMock())
        payload = _payload(tool(
            mode="update", asset_path=ASSET, patch={"patch_id": PATCH_ID, flag: "true"},
        ))
        assert payload["success"] is False, flag
        assert payload["_error"] == "INVALID_PATCH", flag
        assert flag in payload["_message"], flag
        connection.send_command.assert_not_called()


def test_update_conflicting_patch_asset_path_is_rejected_before_forwarding():
    mcp, tool, connection = _register(MagicMock())
    payload = _payload(tool(
        mode="update",
        asset_path=ASSET,
        patch={"asset_path": "/Game/Temp/BP_Other.BP_Other", "patch_id": PATCH_ID},
    ))
    assert payload["success"] is False
    assert payload["_error"] == "INVALID_PATCH"
    assert "asset_path" in payload["_message"]
    connection.send_command.assert_not_called()


def test_update_null_patch_asset_path_is_rejected_before_forwarding():
    """A present null asset_path key is duplicate envelope ownership, not an omission."""
    mcp, tool, connection = _register(MagicMock())
    payload = _payload(tool(
        mode="update",
        asset_path=ASSET,
        patch={"asset_path": None, "patch_id": PATCH_ID},
    ))
    assert payload["success"] is False
    assert payload["_error"] == "INVALID_PATCH"
    assert "asset_path" in payload["_message"]
    connection.send_command.assert_not_called()


def test_update_matching_patch_asset_path_forwards_one_envelope():
    mcp, tool, connection = _register(MagicMock())
    connection.send_command.return_value = {"success": True, "data": _native_data()}
    tool(
        mode="update", asset_path=ASSET, patch={"asset_path": ASSET, "patch_id": PATCH_ID},
    )
    assert _forwarded_request(connection) == {
        "asset_path": ASSET,
        "patch_id": PATCH_ID,
    }


def test_update_forwards_unknown_patch_fields_to_native_validation():
    """The facade never re-implements native semantics: unknown fields reach the envelope."""
    error = UECommandError(
        "graph.apply_patch", "INVALID_FIELD", "Unknown patch request field: bogus",
        {"field": "bogus"},
    )
    mcp, tool, connection = _register(MagicMock())
    connection.send_command.side_effect = error
    payload = _payload(tool(
        mode="update",
        asset_path=ASSET,
        patch={"patch_id": PATCH_ID, "bogus": 1},
    ))
    assert _forwarded_request(connection)["bogus"] == 1
    assert payload["success"] is False
    assert payload["_error"] == "INVALID_FIELD"
    assert payload["field"] == "bogus"


def test_update_passes_structured_native_errors_through_unchanged():
    details = {
        "patch_id": PATCH_ID,
        "changed": True,
        "apply_status": "applied",
        "readback_status": "matched",
        "save_status": "failed",
    }
    mcp, tool, connection = _register(MagicMock())
    connection.send_command.side_effect = UECommandError(
        "graph.apply_patch", "SAVE_FAILED", "saving the package failed", details,
    )
    payload = _payload(tool(
        mode="update", asset_path=ASSET, patch={"patch_id": PATCH_ID, "dry_run": False},
    ))
    assert payload["success"] is False
    assert payload["_error"] == "SAVE_FAILED"
    assert payload["_message"] == "saving the package failed"
    assert payload["_command"] == "graph.apply_patch"
    for key, value in details.items():
        assert payload[key] == value


def test_update_missing_native_command_never_falls_back_to_batch():
    mcp, tool, connection = _register(MagicMock())
    connection.send_command.side_effect = UECommandError(
        "graph.apply_patch", "UNKNOWN_COMMAND", "Unknown graph command: apply_patch", {},
    )
    payload = _payload(tool(
        mode="update", asset_path=ASSET, patch={"patch_id": PATCH_ID, "dry_run": True},
    ))
    assert payload["success"] is False
    assert payload["_error"] == "UNKNOWN_COMMAND"
    assert [call.args[0] for call in connection.send_command.call_args_list] == ["graph.apply_patch"]


def test_update_connection_failure_is_reported_without_legacy_fallback():
    mcp, tool, connection = _register(MagicMock())
    connection.send_command.side_effect = ConnectionError("Lost connection to Unreal Editor: boom")
    raw = tool(
        mode="update", asset_path=ASSET, patch={"patch_id": PATCH_ID, "dry_run": True},
    )
    assert "Lost connection" in raw
    assert [call.args[0] for call in connection.send_command.call_args_list] == ["graph.apply_patch"]


def test_update_returns_compact_native_outcome_without_graph_dump():
    mcp, tool, connection = _register(MagicMock())
    node_guid = "11111111-2222-3333-4444-555555555555"
    connection.send_command.return_value = {
        "success": True,
        "data": _native_data(
            changed=True,
            dry_run=False,
            apply_status="applied",
            compile_status="compiled",
            readback_status="matched",
            save_status="saved",
            post_save_status="verified",
            target_compile_count=1,
            saved=True,
            node_mappings={"print": node_guid},
            locators={"graph_guid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"},
            diagnostics=["compiler warning"],
        ),
    }
    payload = _payload(tool(
        mode="update", asset_path=ASSET, patch={"patch_id": PATCH_ID, "dry_run": False},
    ))
    assert payload["patch_id"] == PATCH_ID
    assert payload["apply_status"] == "applied"
    assert payload["node_mappings"] == {"print": node_guid}
    assert payload["diagnostics"] == ["compiler warning"]
    assert "nodes" not in payload


def test_update_maximal_compact_outcome_survives_the_response_bound():
    """R5: the bounded compact outcome is never reported as an oversized-response failure."""
    mcp, tool, connection = _register(MagicMock())
    fingerprint = {
        "package_saved_hash": "a" * 32,
        "is_dirty": False,
        "dirty_epoch": "3",
        "not_ready": False,
        "graph_authoring_version": 1,
        "graph_authoring_hash": "b" * 32,
    }
    data = _native_data(
        changed=True,
        dry_run=False,
        apply_status="applied",
        compile_status="compiled",
        readback_status="matched",
        save_status="saved",
        post_save_status="verified",
        target_compile_count=1,
        saved=True,
        dirty_before=False,
        dirty_after=False,
        fingerprint_before=dict(fingerprint),
        fingerprint_after=dict(fingerprint),
        reused_client_ids=[],
        node_mappings={f"client_{index:02d}": f"00000000-0000-0000-0000-{index:012d}" for index in range(64)},
        locators={
            "graph_guid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
            "subgraph_path": "BeginPlay.Inner",
            "entry_node_guid": "11111111-2222-3333-4444-555555555555",
            "has_entry_node": True,
        },
        diagnostics=["c" * 512 for _ in range(16)],
    )
    connection.send_command.return_value = {"success": True, "data": data}
    raw = tool(
        mode="update", asset_path=ASSET, patch={"patch_id": PATCH_ID, "dry_run": False},
    )
    payload = _payload(raw)
    assert len(raw) < MAX_RESPONSE_CHARS
    assert "_truncated" not in payload
    assert payload["apply_status"] == "applied"
    assert payload["save_status"] == "saved"
    assert len(payload["node_mappings"]) == 64
    assert len(payload["diagnostics"]) == 16


def test_create_mode_keeps_running_the_create_batch_and_never_patches():
    connection = MagicMock()
    connection.send_command.return_value = {
        "success": True,
        "data": {
            "results": [
                {"index": 0, "success": True, "data": {"asset_path": "/Game/BP_Created"}, "timing_ms": 1},
            ],
            "total_timing_ms": 1,
        },
    }
    mcp = Capture()
    register_blueprint_compose_tools(mcp, connection)
    payload = _payload(mcp.tools["blueprint_compose"](name="BP_Created", path="/Game/"))
    assert payload["success"] is True
    assert payload["asset_path"] == "/Game/BP_Created"
    sent = [call.args[0] for call in connection.send_command.call_args_list]
    assert sent[0] == "batch"
    assert "graph.apply_patch" not in sent


def test_create_mode_rejects_a_patch_without_calling_the_editor():
    mcp, tool, connection = _register(MagicMock())
    payload = _payload(tool(
        name="BP_Created", path="/Game/", patch={"patch_id": PATCH_ID},
    ))
    assert payload["success"] is False
    assert payload["_error"] == "INVALID_PATCH"
    assert "mode='update'" in payload["_message"]
    connection.send_command.assert_not_called()


def test_unknown_mode_is_refused_without_calling_the_editor():
    mcp, tool, connection = _register(MagicMock())
    payload = _payload(tool(mode="append", asset_path=ASSET))
    assert payload["success"] is False
    assert payload["_error"] == "UNSUPPORTED_MODE"
    connection.send_command.assert_not_called()
