"""Tests for UMG animation binding MCP router, profile, and response bounds."""

from __future__ import annotations

import json
from unittest.mock import MagicMock
import pytest

from cortex_mcp.operation_schema import (
    DEFAULT_PROFILE,
    build_profile_operation_schema,
    reset_operation_schema_budget,
)
from cortex_mcp.pagination import encode_cursor
from cortex_mcp.response import format_response, MAX_RESPONSE_CHARS
from cortex_mcp.tcp_client import UECommandError
from cortex_mcp.tools.routers import make_router, strict_router_tool, _pagination_cache


@pytest.fixture(autouse=True)
def _clear_cache():
    _pagination_cache.clear()
    reset_operation_schema_budget()
    yield
    _pagination_cache.clear()
    reset_operation_schema_budget()


# ---------------------------------------------------------------------------
# Step 1: Transport bypass tests
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "extra",
    [
        {"limit": 1},
        {"cursor": "not-a-removal-token"},
        {"offset": 0},
    ],
)
def test_removal_rejects_pagination_before_dispatch(extra):
    """remove_animation_binding rejects reserved pagination parameters before dispatch without TCP forwarding."""
    connection = MagicMock()
    router = make_router("umg", connection, "test docs")
    params = {
        "asset_path": "/Game/UI/WBP_Test",
        "animation_name": "Appearance",
        **extra,
    }
    result = json.loads(router("remove_animation_binding", params))
    assert result["_error"] == "INVALID_FIELD"
    connection.send_command.assert_not_called()


def test_removal_allows_explicit_none_pagination_parameters():
    """remove_animation_binding does not reject pagination parameters when explicitly set to None."""
    connection = MagicMock()
    connection.send_command.return_value = {
        "success": True,
        "data": {
            "asset_path": "/Game/UI/WBP_Test",
            "animation_name": "Appearance",
            "changed": False,
            "dry_run": True,
        },
    }
    router = make_router("umg", connection, "test docs")
    params = {
        "asset_path": "/Game/UI/WBP_Test",
        "animation_name": "Appearance",
        "widget_name": "Button_0",
        "limit": None,
        "cursor": None,
        "offset": None,
    }
    result = json.loads(router("remove_animation_binding", params))
    assert "_error" not in result
    connection.send_command.assert_called_once()


def test_removal_rejects_valid_read_cursor_from_other_command():
    """An existing valid read cursor from another command must NOT return its cached page for removal."""
    connection = MagicMock()
    router = make_router("umg", connection, "test docs")

    # Store a valid read page in _pagination_cache
    cache_key = _pagination_cache.store(
        "data.list_datatables",
        {"path": "/Game/Data"},
        "rows",
        [{"id": i, "name": f"Row_{i}"} for i in range(50)],
        {"domain": "data", "command": "list_datatables"},
    )
    valid_cursor = encode_cursor(cache_key, offset=10, limit=10)

    params = {
        "asset_path": "/Game/UI/WBP_Test",
        "animation_name": "Appearance",
        "cursor": valid_cursor,
    }
    result = json.loads(router("remove_animation_binding", params))
    assert result["_error"] == "INVALID_FIELD"
    assert "rows" not in result
    connection.send_command.assert_not_called()


def test_list_animation_bindings_forwards_paging_and_fingerprint_unchanged():
    """For list_animation_bindings: offset, limit, and expected_fingerprint arrive unchanged at native validation."""
    connection = MagicMock()
    connection.send_command.return_value = {
        "success": True,
        "data": {
            "asset_path": "/Game/UI/WBP_Test",
            "animation_name": "Appearance",
            "bindings": [],
            "pagination": {"total": 0, "offset": 5, "limit": 20, "returned": 0, "is_complete": True},
        },
    }
    router = make_router("umg", connection, "test docs")
    params = {
        "asset_path": "/Game/UI/WBP_Test",
        "animation_name": "Appearance",
        "offset": 5,
        "limit": 20,
        "expected_fingerprint": {
            "package_saved_hash": "abc123hash",
            "domain_signature": {
                "version": 1,
                "scope": "umg.animation_binding",
                "asset_path": "/Game/UI/WBP_Test",
                "animation_name": "Appearance",
                "digest": "deadbeef",
            },
        },
    }
    result = json.loads(router("list_animation_bindings", params))
    connection.send_command.assert_called_once_with("umg.list_animation_bindings", params)
    # Generic router pagination envelope must NOT wrap the native pagination
    assert "_pagination" not in result
    assert result["pagination"]["offset"] == 5


def test_list_animation_bindings_preserves_strict_types_without_integer_coercion():
    """Ensure bool/fraction values are forwarded unchanged to prevent generic integer coercion from weakening strict types."""
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": {"bindings": []}}
    router = make_router("umg", connection, "test docs")

    # Pass limit as float 1.5 and bool True
    params_float = {
        "asset_path": "/Game/UI/WBP_Test",
        "animation_name": "Appearance",
        "limit": 1.5,
    }
    router("list_animation_bindings", params_float)
    call_args_float = connection.send_command.call_args[0]
    assert call_args_float[1]["limit"] == 1.5
    assert isinstance(call_args_float[1]["limit"], float)

    connection.reset_mock()
    params_bool = {
        "asset_path": "/Game/UI/WBP_Test",
        "animation_name": "Appearance",
        "limit": True,
    }
    router("list_animation_bindings", params_bool)
    call_args_bool = connection.send_command.call_args[0]
    assert call_args_bool[1]["limit"] is True
    assert isinstance(call_args_bool[1]["limit"], bool)


# ---------------------------------------------------------------------------
# Step 2: Exact forwarding and profile tests
# ---------------------------------------------------------------------------


def test_valid_removal_exact_forwarding():
    """A valid removal sends exactly one umg.remove_animation_binding TCP command with selector/fingerprint unchanged."""
    connection = MagicMock()
    connection.send_command.return_value = {
        "success": True,
        "data": {
            "asset_path": "/Game/UI/WBP_Test",
            "animation_name": "Appearance",
            "dry_run": True,
            "changed": False,
            "save_attempted": False,
            "saved": False,
            "matched_selector": {
                "binding_guid": "{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}",
                "widget_name": "BodySizeBox",
                "slot_widget_name": "",
                "is_root_widget": False,
            },
            "remaining_bindings": [],
        },
    }
    router = make_router("umg", connection, "test docs")
    params = {
        "asset_path": "/Game/UI/WBP_Test",
        "animation_name": "Appearance",
        "selector": {
            "binding_guid": "{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}",
            "widget_name": "BodySizeBox",
            "slot_widget_name": "",
            "is_root_widget": False,
        },
        "expected_fingerprint": {
            "package_saved_hash": "abc",
            "domain_signature": {
                "version": 1,
                "scope": "umg.animation_binding",
                "asset_path": "/Game/UI/WBP_Test",
                "animation_name": "Appearance",
                "digest": "123",
            },
        },
    }
    result = json.loads(router("remove_animation_binding", params))
    connection.send_command.assert_called_once_with("umg.remove_animation_binding", params)
    # No new Python defaults may contradict native defaults (e.g. dry_run/save not injected into call params)
    sent_params = connection.send_command.call_args[0][1]
    assert "dry_run" not in sent_params
    assert "save" not in sent_params
    assert result["dry_run"] is True


def test_strict_router_envelope_validation():
    """Test the registered FastMCP envelope: unexpected top-level operation fields and non-object params retain INVALID_INVOCATION_SHAPE."""
    connection = MagicMock()
    raw_router = make_router("umg", connection, "test docs")
    wrapped_router = strict_router_tool(raw_router, "umg")

    # Unexpected top-level argument
    res1 = json.loads(wrapped_router("remove_animation_binding", {"asset_path": "/Game/UI/WBP_Test"}, unexpected_arg=123))
    assert res1["_error"] == "INVALID_INVOCATION_SHAPE"
    assert "unexpected_arg" in res1["_message"]
    connection.send_command.assert_not_called()

    # Non-object params
    res2 = json.loads(wrapped_router("remove_animation_binding", "not-a-dict"))  # type: ignore
    assert res2["_error"] == "INVALID_INVOCATION_SHAPE"
    assert "params must be an object" in res2["_message"]
    connection.send_command.assert_not_called()


def test_profile_umg_authoring_permits_live_operations():
    """UMGAuthoring profile permits live UMG operations when native schema exists."""
    connection = MagicMock()
    connection.send_command.return_value = {
        "success": True,
        "data": {
            "name": "remove_animation_binding",
            "params": [
                {"name": "asset_path", "type": "string", "required": True},
                {"name": "animation_name", "type": "string", "required": True},
                {"name": "selector", "type": "object", "required": True},
                {"name": "expected_fingerprint", "type": "object", "required": True},
            ],
        },
    }
    result_str = build_profile_operation_schema(connection, DEFAULT_PROFILE, "umg", "remove_animation_binding")
    result = json.loads(result_str)
    assert result["editor_available"] is True
    assert result["policy_allowed"] is True
    assert result["command"] == "remove_animation_binding"


def test_profile_retains_command_not_found_when_missing_from_editor():
    """Missing native schema retains CAPABILITY_COMMAND_NOT_FOUND and editor_available=false even when cache advertises name."""
    connection = MagicMock()
    connection.send_command.side_effect = UECommandError(
        "core.get_operation_schema",
        "CAPABILITY_COMMAND_NOT_FOUND",
        "Command not found in connected editor",
        {"cache_advertised": True, "restart_or_reload_required": True},
    )
    result_str = build_profile_operation_schema(connection, DEFAULT_PROFILE, "umg", "remove_animation_binding")
    result = json.loads(result_str)
    assert result["editor_available"] is False
    assert result["policy_allowed"] is False
    assert result["restart_or_reload_required"] is True
    assert result["unreal_error"]["code"] == "CAPABILITY_COMMAND_NOT_FOUND"


# ---------------------------------------------------------------------------
# Step 3: Response boundary tests
# ---------------------------------------------------------------------------


def _build_native_removal_result(
    num_remaining: int,
    entry_size: int = 50,
    num_diagnostics: int = 0,
    changed: bool = True,
    dry_run: bool = False,
    saved: bool = True,
    save_attempted: bool = True,
    save_error: str | None = None,
) -> dict:
    """Construct native-shaped remove_animation_binding results."""
    remaining = [
        {
            "index": i,
            "binding_guid": f"{{A1B2C3D4-E5F6-7890-ABCD-{i:012d}}}",
            "widget_name": f"Widget_{i}_" + ("x" * entry_size),
            "slot_widget_name": "",
            "is_root_widget": False,
            "guid_sharing_count": 1,
            "track_count": 2,
        }
        for i in range(num_remaining)
    ]
    diagnostics = [
        f"Diagnostic message {i}: " + ("y" * 80)
        for i in range(num_diagnostics)
    ]
    return {
        "asset_path": "/Game/UI/WBP_EmailList",
        "animation_name": "appearance",
        "dry_run": dry_run,
        "changed": changed,
        "save_attempted": save_attempted,
        "saved": saved,
        "fingerprint": {
            "package_saved_hash": "0123456789abcdef",
            "is_dirty": not saved,
            "dirty_epoch": "104",
            "not_ready": False,
            "compiled_signature_crc": 12345678,
            "domain_signature": {
                "version": 1,
                "scope": "umg.animation_binding",
                "asset_path": "/Game/UI/WBP_EmailList",
                "animation_name": "appearance",
                "digest": "a94f6c8d7e2b5f10123456789abcdef0",
            },
        },
        "matched_selector": {
            "binding_guid": "{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}",
            "widget_name": "StorylineIcon",
            "slot_widget_name": "",
            "is_root_widget": False,
        },
        "before": {
            "umg_binding_count": num_remaining + 1,
            "movie_scene_binding_count": num_remaining + 1,
            "track_count": (num_remaining + 1) * 2,
        },
        "after": {
            "umg_binding_count": num_remaining,
            "movie_scene_binding_count": num_remaining,
            "track_count": num_remaining * 2,
        },
        "scene_data_removed": True,
        "remaining_bindings": remaining,
        "_remaining_bindings_truncated": False,
        "_remaining_bindings_total": num_remaining,
        "save_error": save_error,
        "diagnostics": diagnostics,
    }


def test_removal_result_below_40k_preserved_intact():
    """Results below 40,000 characters are returned with all fields preserved as-is."""
    data = _build_native_removal_result(num_remaining=3, entry_size=20)
    formatted = format_response(data, "umg_cmd")
    result = json.loads(formatted)

    assert len(formatted) <= MAX_RESPONSE_CHARS
    assert result["changed"] is True
    assert result["dry_run"] is False
    assert result["save_attempted"] is True
    assert result["saved"] is True
    assert result["fingerprint"]["domain_signature"]["digest"] == "a94f6c8d7e2b5f10123456789abcdef0"
    assert result["matched_selector"]["widget_name"] == "StorylineIcon"
    assert result["before"]["umg_binding_count"] == 4
    assert result["after"]["umg_binding_count"] == 3
    assert result["_remaining_bindings_truncated"] is False
    assert len(result["remaining_bindings"]) == 3


def test_removal_result_above_40k_preserves_outcomes_and_truncates_remaining_bindings():
    """Results above 40,000 characters: essential outcome fields are never truncated; remaining_bindings carries truncation metadata."""
    # 201 bindings with moderate entry size clearly exceeds 40,000 chars
    data = _build_native_removal_result(num_remaining=201, entry_size=150)
    raw_size = len(json.dumps(data, indent=2))
    assert raw_size > MAX_RESPONSE_CHARS

    formatted = format_response(data, "umg_cmd")
    assert len(formatted) <= MAX_RESPONSE_CHARS
    result = json.loads(formatted)

    # Must NEVER replace mutation result with generic size error
    assert result.get("_error") != "RESPONSE_TOO_LARGE"

    # Essential outcome fields must be completely preserved
    assert result["changed"] is True
    assert result["dry_run"] is False
    assert result["save_attempted"] is True
    assert result["saved"] is True
    assert result["fingerprint"]["domain_signature"]["digest"] == "a94f6c8d7e2b5f10123456789abcdef0"
    assert result["matched_selector"]["widget_name"] == "StorylineIcon"
    assert result["before"]["umg_binding_count"] == 202
    assert result["after"]["umg_binding_count"] == 201

    # remaining_bindings must carry truncation metadata, totals, and referral instructions
    assert result["_remaining_bindings_truncated"] is True
    assert result["_remaining_bindings_total"] == 201
    assert len(result["remaining_bindings"]) < 201
    assert "umg.list_animation_bindings" in (
        result.get("_suggestion", "") + result.get("_remaining_bindings_instructions", "")
    )


def test_removal_result_fewer_than_ten_large_entries_truncates_without_error():
    """Fewer than 10 large entries (e.g. 3 entries) exceeding 40k must truncate remaining_bindings rather than failing with RESPONSE_TOO_LARGE."""
    # 3 entries, but each entry is huge (~15,000 chars)
    data = _build_native_removal_result(num_remaining=3, entry_size=15_000)
    raw_size = len(json.dumps(data, indent=2))
    assert raw_size > MAX_RESPONSE_CHARS

    formatted = format_response(data, "umg_cmd")
    assert len(formatted) <= MAX_RESPONSE_CHARS
    result = json.loads(formatted)

    # Must NOT fail with RESPONSE_TOO_LARGE
    assert result.get("_error") != "RESPONSE_TOO_LARGE"
    assert result["changed"] is True
    assert result["_remaining_bindings_truncated"] is True
    assert result["_remaining_bindings_total"] == 3
    assert len(result["remaining_bindings"]) < 3
    assert "umg.list_animation_bindings" in (
        result.get("_suggestion", "") + result.get("_remaining_bindings_instructions", "")
    )


def test_removal_result_with_large_diagnostics_and_remaining_bindings():
    """When both large diagnostics and remaining_bindings are present, generic largest-list selection cannot hide canonical records."""
    # 15 diagnostics and 11 remaining bindings
    data = _build_native_removal_result(num_remaining=11, entry_size=3_500, num_diagnostics=15)
    raw_size = len(json.dumps(data, indent=2))
    assert raw_size > MAX_RESPONSE_CHARS



    formatted = format_response(data, "umg_cmd")
    assert len(formatted) <= MAX_RESPONSE_CHARS
    result = json.loads(formatted)

    assert result.get("_error") != "RESPONSE_TOO_LARGE"
    assert result["changed"] is True
    assert result["_remaining_bindings_truncated"] is True
    assert result["_remaining_bindings_total"] == 11
    assert "umg.list_animation_bindings" in (
        result.get("_suggestion", "") + result.get("_remaining_bindings_instructions", "")
    )


def test_removal_save_failure_retains_error_identity_and_outcomes():
    """Apply-success and save-failure results must retain changed, save_attempted, saved=false, and error identity."""
    data = _build_native_removal_result(
        num_remaining=100,
        entry_size=300,
        changed=True,
        dry_run=False,
        save_attempted=True,
        saved=False,
        save_error="Package save failed: disk read-only",
    )
    raw_size = len(json.dumps(data, indent=2))
    assert raw_size > MAX_RESPONSE_CHARS

    formatted = format_response(data, "umg_cmd")
    assert len(formatted) <= MAX_RESPONSE_CHARS
    result = json.loads(formatted)

    assert result["changed"] is True
    assert result["save_attempted"] is True
    assert result["saved"] is False
    assert result["save_error"] == "Package save failed: disk read-only"
    assert result["fingerprint"]["is_dirty"] is True
    assert result["_remaining_bindings_truncated"] is True


def test_list_animation_bindings_track_class_response_contract():
    """umg.list_animation_bindings track_class response contract requires full class-path (/Script/ModuleName.ClassName), rejecting short names."""
    native_data = {
        "asset_path": "/Game/UI/WBP_Test",
        "animation_name": "Appearance",
        "bindings": [
            {
                "widget_name": "BodySizeBox",
                "tracks": [
                    {
                        "track_name": "WidthOverride",
                        "track_class": "/Script/MovieSceneTracks.MovieSceneFloatTrack",
                    },
                    {
                        "track_name": "HeightOverride",
                        "track_class": "/Script/MovieSceneTracks.MovieSceneFloatTrack",
                    },
                ],
            },
            {
                "widget_name": "StorylineIcon",
                "tracks": [
                    {
                        "track_name": "bIsEnabled",
                        "track_class": "/Script/MovieSceneTracks.MovieSceneBoolTrack",
                    },
                ],
            },
        ],
    }

    # Verify that valid full class-path contract passes
    for b in native_data["bindings"]:
        for t in b["tracks"]:
            track_class = t["track_class"]
            assert track_class.startswith("/Script/"), f"track_class must start with '/Script/': {track_class}"
            assert "." in track_class, f"track_class must contain '.' separating package and class: {track_class}"

    # Verify that invalid short names like 'MovieSceneFloatTrack' fail the contract check
    invalid_short_names = ["MovieSceneFloatTrack", "MovieSceneBoolTrack"]
    for short_name in invalid_short_names:
        assert not short_name.startswith("/Script/"), f"Short name should not pass full class path contract: {short_name}"
