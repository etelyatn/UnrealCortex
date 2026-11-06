"""Tests for UMG animation binding MCP router, profile, and response bounds."""

from __future__ import annotations

import json
from unittest.mock import MagicMock
import pytest

from cortex_mcp.operation_schema import (
    DEFAULT_PROFILE,
    build_profile_operation_schema,
)
from cortex_mcp.pagination import encode_cursor
from cortex_mcp.response import compact_json, format_response, MAX_RESPONSE_CHARS
from cortex_mcp.tcp_client import UECommandError
from cortex_mcp.tools.routers import make_router, strict_router_tool, _pagination_cache


@pytest.fixture(autouse=True)
def _clear_cache():
    _pagination_cache.clear()
    yield
    _pagination_cache.clear()


# ---------------------------------------------------------------------------
# Helpers for animation authoring boundary coverage
# ---------------------------------------------------------------------------

_SCRATCH_ASSET = "/Game/UI/Scratch"
_ANIMATION_NAME = "Fade"
_BINDING_GUID = "{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}"
_FINGERPRINT_DIGEST = "a94f6c8d7e2b5f10123456789abcdef0"
_AUTHORING_COMMANDS = ("ensure_animation_binding", "set_animation_property_track")


def _strict_umg_router(connection):
    """Registered strict umg_cmd envelope over the real router/formatter path."""
    return strict_router_tool(make_router("umg", connection, "test docs"), "umg")


def _v2_fingerprint() -> dict:
    """umg.animation_binding guard signature at version 2."""
    return {
        "package_saved_hash": "0123456789abcdef",
        "is_dirty": True,
        "dirty_epoch": "104",
        "not_ready": False,
        "compiled_signature_crc": 12345678,
        "domain_signature": {
            "version": 2,
            "scope": "umg.animation_binding",
            "asset_path": _SCRATCH_ASSET,
            "animation_name": _ANIMATION_NAME,
            "digest": _FINGERPRINT_DIGEST,
        },
    }


def _oversized_authored_track(key_count: int = 500) -> dict:
    """Normalized setter readback large enough that the whole authoring result exceeds 40k."""
    keys = [
        {
            "frame_number": frame,
            "time_seconds": round(frame / 30.0, 6),
            "value": round(frame / key_count, 6),
            "interpolation": "linear",
        }
        for frame in range(key_count)
    ]
    return {
        "property_path": "RenderOpacity",
        "type": "float",
        "sections": [
            {
                "start_seconds": 0.0,
                "end_seconds": 20.0,
                "evaluation": {"blend_type": "Absolute", "completion_mode": "RestoreState"},
                "channels": [
                    {
                        "channel": "float",
                        "default_value": None,
                        "pre_infinity_extrapolation": "constant",
                        "post_infinity_extrapolation": "constant",
                        "keys": keys,
                    }
                ],
            }
        ],
    }


def _detailed_track(index: int, keys_per_track: int) -> dict:
    """One float track as returned by an include_track_content=true detailed read."""
    property_path = f"FloatProperty_{index}"
    keys = [
        {
            "frame_number": frame,
            "time_seconds": round(frame / 30.0, 6),
            "value": round(frame / max(keys_per_track, 1), 6),
            "interpolation": "linear",
        }
        for frame in range(keys_per_track)
    ]
    return {
        "track_name": property_path,
        "track_class": "/Script/MovieSceneTracks.MovieSceneFloatTrack",
        "property_path": property_path,
        "property_name": property_path,
        "type": "float",
        "sections": [
            {
                "lower_bound": {"value": 0, "type": "Inclusive"},
                "upper_bound": {"value": 240 * keys_per_track, "type": "Exclusive"},
                "lower_seconds": 0.0,
                "upper_seconds": round(keys_per_track / 30.0, 6),
                "evaluation": {
                    "blend_type": "Absolute",
                    "completion_mode": "RestoreState",
                    "is_active": True,
                    "is_locked": False,
                    "pre_roll_frames": 0,
                    "post_roll_frames": 0,
                },
                "channels": [
                    {
                        "channel": "float",
                        "default_value": None,
                        "pre_infinity_extrapolation": "constant",
                        "post_infinity_extrapolation": "constant",
                        "keys": keys,
                    }
                ],
                "tick_resolution": {"numerator": 24000, "denominator": 1},
            }
        ],
    }


def _detailed_read_payload(track_count: int, keys_per_track: int) -> dict:
    """Native-shaped include_track_content page for a single animation binding."""
    return {
        "asset_path": _SCRATCH_ASSET,
        "animation_name": _ANIMATION_NAME,
        "fingerprint": _v2_fingerprint(),
        "playback_range": {
            "lower_bound": {"value": 0, "type": "Inclusive"},
            "upper_bound": {"value": 240 * keys_per_track, "type": "Exclusive"},
            "length_seconds": round(keys_per_track / 30.0, 6),
        },
        "tick_resolution": {"numerator": 24000, "denominator": 1},
        "display_rate": {"numerator": 30, "denominator": 1},
        "umg_binding_count": 1,
        "movie_scene_binding_count": 1,
        "track_count": track_count,
        "bindings": [
            {
                "index": 0,
                "binding_guid": _BINDING_GUID,
                "widget_name": "Decor",
                "slot_widget_name": "",
                "is_root_widget": False,
                "target_exists": True,
                "slot_exists": False,
                "possessable_exists": True,
                "guid_sharing_count": 1,
                "track_count": track_count,
                "tracks": [_detailed_track(index, keys_per_track) for index in range(track_count)],
            }
        ],
        "pagination": {
            "total": 1,
            "offset": 0,
            "limit": 50,
            "returned": 1,
            "next_offset": None,
            "is_complete": True,
        },
        "diagnostics": [],
    }


def _list_read_result(payload: dict, include_track_content: bool | None = None) -> dict:
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": payload}
    router = _strict_umg_router(connection)
    params = {"asset_path": _SCRATCH_ASSET, "animation_name": _ANIMATION_NAME}
    if include_track_content is not None:
        params["include_track_content"] = include_track_content
    return json.loads(router("list_animation_bindings", params))


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


# ---------------------------------------------------------------------------
# Step 1b: Animation authoring parameter boundary
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("command", _AUTHORING_COMMANDS)
@pytest.mark.parametrize(
    "extra",
    [{"limit": None}, {"offset": None}, {"cursor": None}, {"limit": True}, {"limit": 1.5}],
)
def test_authoring_rejects_supplied_pagination(command, extra):
    """Any supplied cursor/offset/limit field, including null, refuses before cached pagination or TCP forwarding."""
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": {"changed": True}}
    router = _strict_umg_router(connection)
    result = json.loads(
        router(command, {"asset_path": _SCRATCH_ASSET, "animation_name": _ANIMATION_NAME, **extra})
    )
    assert result.get("_error") == "INVALID_FIELD"
    assert "rows" not in result
    connection.send_command.assert_not_called()


@pytest.mark.parametrize("command", _AUTHORING_COMMANDS)
def test_authoring_rejects_valid_read_cursor_from_unrelated_cache(command):
    """A live cursor produced by another read never returns that cached page to an animation writer."""
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": {"changed": True}}
    router = _strict_umg_router(connection)
    cache_key = _pagination_cache.store(
        "data.list_datatables",
        {"path": "/Game/Data"},
        "rows",
        [{"id": index, "name": f"Row_{index}"} for index in range(50)],
        {"domain": "data", "command": "list_datatables"},
    )
    cursor = encode_cursor(cache_key, offset=10, limit=10)
    result = json.loads(
        router(
            command,
            {"asset_path": _SCRATCH_ASSET, "animation_name": _ANIMATION_NAME, "cursor": cursor},
        )
    )
    assert result.get("_error") == "INVALID_FIELD"
    assert "rows" not in result
    connection.send_command.assert_not_called()


@pytest.mark.parametrize("flag", [None, 0, 1, "true", "false", [], {}])
def test_detailed_read_rejects_non_boolean_include_track_content(flag):
    """include_track_content is a strict boolean validated before dispatch; malformed values never fall through to a summary read."""
    connection = MagicMock()
    connection.send_command.return_value = {
        "success": True,
        "data": {
            "asset_path": _SCRATCH_ASSET,
            "animation_name": _ANIMATION_NAME,
            "bindings": [],
        },
    }
    router = _strict_umg_router(connection)
    result = json.loads(
        router(
            "list_animation_bindings",
            {
                "asset_path": _SCRATCH_ASSET,
                "animation_name": _ANIMATION_NAME,
                "include_track_content": flag,
            },
        )
    )
    assert result.get("_error") == "INVALID_FIELD"
    connection.send_command.assert_not_called()


# ---------------------------------------------------------------------------
# Step 2: Profile tests
# ---------------------------------------------------------------------------


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
    raw_size = len(compact_json(data))
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
    raw_size = len(compact_json(data))
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
    raw_size = len(compact_json(data))
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
    raw_size = len(compact_json(data))
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


def test_summary_read_keeps_established_shape_without_detail_envelope():
    """Without include_track_content the established summary read is unchanged: native pagination and per-track counts survive."""
    payload = _detailed_read_payload(track_count=6, keys_per_track=2)
    payload["bindings"][0]["tracks"] = [
        {
            "track_name": track["property_path"],
            "track_class": track["track_class"],
            "section_count": 1,
            "channel_count": 1,
            "key_count": 2,
        }
        for track in payload["bindings"][0]["tracks"]
    ]
    result = _list_read_result(payload)
    assert "_error" not in result
    assert result["umg_binding_count"] == 1
    assert result["track_count"] == 6
    assert result["pagination"] == {
        "total": 1,
        "offset": 0,
        "limit": 50,
        "returned": 1,
        "next_offset": None,
        "is_complete": True,
    }
    assert [track["track_name"] for track in result["bindings"][0]["tracks"]] == [
        f"FloatProperty_{index}" for index in range(6)
    ]


def test_detailed_read_complete_page_retains_every_track():
    """A detailed read that fits returns the complete page with reader_complete=true and no silent track cutting."""
    payload = _detailed_read_payload(track_count=6, keys_per_track=2)
    assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS
    result = _list_read_result(payload, include_track_content=True)
    assert "_error" not in result
    assert result["reader_complete"] is True
    assert result["pagination"]["is_complete"] is True
    binding = result["bindings"][0]
    assert len(binding["tracks"]) == 6
    assert "_tracks_truncated" not in binding
    assert binding["tracks"][0]["property_path"] == "FloatProperty_0"
    assert binding["tracks"][5]["sections"][0]["channels"][0]["keys"][0]["frame_number"] == 0


def test_many_track_detailed_read_reports_explicit_incompleteness():
    """Six detailed tracks over 40k must not be silently cut into a page that still claims completion."""
    payload = _detailed_read_payload(track_count=6, keys_per_track=90)
    assert len(compact_json(payload)) > MAX_RESPONSE_CHARS
    result = _list_read_result(payload, include_track_content=True)
    assert result.get("_error") == "RESPONSE_TOO_LARGE"
    assert result["reader_complete"] is False
    assert result.get("pagination", {}).get("is_complete") is not True
    assert "bindings" not in result
    assert result["asset_path"] == _SCRATCH_ASSET
    assert result["animation_name"] == _ANIMATION_NAME
    assert result["fingerprint"] == _v2_fingerprint()
    assert result["summary_counts"] == {
        "umg_binding_count": 1,
        "movie_scene_binding_count": 1,
        "track_count": 6,
    }
    assert result["max_response_chars"] == MAX_RESPONSE_CHARS


def test_single_oversized_detailed_binding_reports_explicit_incompleteness():
    """A single binding whose details exceed 40k yields the bounded incomplete envelope with retained summary facts."""
    payload = _detailed_read_payload(track_count=1, keys_per_track=700)
    assert len(compact_json(payload)) > MAX_RESPONSE_CHARS
    result = _list_read_result(payload, include_track_content=True)
    assert result.get("_error") == "RESPONSE_TOO_LARGE"
    assert result["reader_complete"] is False
    assert result.get("pagination", {}).get("is_complete") is not True
    assert "bindings" not in result
    assert result["asset_path"] == _SCRATCH_ASSET
    assert result["animation_name"] == _ANIMATION_NAME
    assert result["fingerprint"] == _v2_fingerprint()
    assert result["summary_counts"] == {
        "umg_binding_count": 1,
        "movie_scene_binding_count": 1,
        "track_count": 1,
    }
    assert result["max_response_chars"] == MAX_RESPONSE_CHARS


def test_oversized_applied_authoring_result_preserves_outcome_and_fingerprint():
    """An oversized applied setter result keeps its outcome; only the detailed authored track is omitted and disclosed."""
    fingerprint = _v2_fingerprint()
    selector = {
        "binding_guid": _BINDING_GUID,
        "widget_name": "Decor",
        "slot_widget_name": "",
        "is_root_widget": False,
    }
    before = {"umg_binding_count": 1, "movie_scene_binding_count": 1, "track_count": 0}
    after = {"umg_binding_count": 1, "movie_scene_binding_count": 1, "track_count": 1}
    native = {
        "asset_path": _SCRATCH_ASSET,
        "animation_name": _ANIMATION_NAME,
        "dry_run": False,
        "changed": True,
        "would_change": True,
        "fingerprint": fingerprint,
        "matched_selector": selector,
        "property_path": "RenderOpacity",
        "reader_complete": True,
        "before": before,
        "after": after,
        "authored_track": _oversized_authored_track(),
    }
    assert len(compact_json(native)) > MAX_RESPONSE_CHARS

    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": native}
    router = _strict_umg_router(connection)
    text = router(
        "set_animation_property_track",
        {
            "asset_path": _SCRATCH_ASSET,
            "animation_name": _ANIMATION_NAME,
            "selector": selector,
            "property_path": "RenderOpacity",
            "track": {"type": "float", "sections": []},
            "expected_fingerprint": fingerprint,
        },
    )
    result = json.loads(text)

    assert len(text) <= MAX_RESPONSE_CHARS
    assert "_error" not in result
    assert result["asset_path"] == _SCRATCH_ASSET
    assert result["animation_name"] == _ANIMATION_NAME
    assert result["dry_run"] is False
    assert result["changed"] is True
    assert result["would_change"] is True
    assert result["fingerprint"] == fingerprint
    assert result["matched_selector"] == selector
    assert result["property_path"] == "RenderOpacity"
    assert result["before"] == before
    assert result["after"] == after
    assert result["reader_complete"] is False
    assert result["authored_track_omitted"] is True
    assert "authored_track" not in result
    assert "umg.list_animation_bindings" in text


@pytest.mark.parametrize("command", _AUTHORING_COMMANDS)
def test_oversized_authoring_error_payload_never_loses_identity_or_current_guard(command):
    """MCP overflow path: an oversized authoring error payload keeps its code, message and current guard.

    The oversized echo is synthesized here on purpose. The native verifier bounds the echo of an
    untrusted expected value, so this is an MCP-side overflow probe, not a claim about native
    output. Response size is deliberately not asserted: an oversized error payload cannot both be
    retained verbatim and fit 40,000 characters.
    """
    current_fingerprint = _v2_fingerprint()
    message = "Untrusted stale-state diagnostic: " + "e" * (MAX_RESPONSE_CHARS + 4096)
    assert len(message) > MAX_RESPONSE_CHARS

    connection = MagicMock()
    connection.send_command.side_effect = UECommandError(
        f"umg.{command}",
        "STALE_PRECONDITION",
        message,
        {"current_fingerprint": current_fingerprint},
    )
    router = _strict_umg_router(connection)
    result = json.loads(
        router(command, {"asset_path": _SCRATCH_ASSET, "animation_name": _ANIMATION_NAME})
    )

    assert result["_error"] == "STALE_PRECONDITION"
    assert result["_message"] == message
    assert result["current_fingerprint"] == current_fingerprint
    assert "fingerprint" not in result


def test_native_incomplete_detail_keeps_summary_counts_and_guard():
    """Native budget refusal remains useful for recovery instead of losing its summary."""
    counts = {"umg_binding_count": 1, "movie_scene_binding_count": 1, "track_count": 2}
    native = {
        "_error": "RESPONSE_TOO_LARGE",
        "reader_complete": False,
        "asset_path": _SCRATCH_ASSET,
        "animation_name": _ANIMATION_NAME,
        "fingerprint": _v2_fingerprint(),
        "summary_counts": counts,
        "max_response_chars": MAX_RESPONSE_CHARS,
    }
    result = _list_read_result(native, include_track_content=True)
    assert result["_error"] == "RESPONSE_TOO_LARGE"
    assert result["reader_complete"] is False
    assert result["summary_counts"] == counts
    assert result["fingerprint"] == native["fingerprint"]
    assert "bindings" not in result
