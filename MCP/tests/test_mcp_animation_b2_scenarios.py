"""Live animation Phase B2 scenario checks against the CortexSandbox editor."""

from __future__ import annotations

import json
import uuid

import pytest


async def call_tool(client, name: str, args: dict) -> dict:
    result = await client.call_tool(name, args)
    return json.loads(result.content[0].text)


def _uniq(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex[:8]}"


@pytest.mark.anyio
@pytest.mark.scenario
async def test_scenario_animation_phase_b2_dry_run_readback(mcp_client):
    """Inspect one sequence, montage, and skeleton, then dry-run each B2 family."""
    status = await call_tool(mcp_client, "core_cmd", {"command": "get_status", "params": {}})
    assert status["project_name"] == "CortexSandbox"

    async def first_asset(asset_type: str) -> str | None:
        assets = await call_tool(mcp_client, "anim_cmd", {
            "command": "list_assets",
            "params": {"asset_type": asset_type, "path": "/Game", "limit": 20},
        })
        items = assets.get("assets", {}).get("items", [])
        return items[0]["asset_path"] if items else None

    sequence_path = await first_asset("AnimSequence")
    montage_path = await first_asset("AnimMontage")
    skeleton_path = await first_asset("Skeleton")
    if not sequence_path or not montage_path or not skeleton_path:
        pytest.skip("CortexSandbox animation benchmark assets are incomplete")

    sequence = await call_tool(mcp_client, "anim_cmd", {
        "command": "get_sequence_info",
        "params": {"asset_path": sequence_path, "notify_limit": 20},
    })
    montage = await call_tool(mcp_client, "anim_cmd", {
        "command": "get_montage_info",
        "params": {"asset_path": montage_path, "section_limit": 20},
    })
    skeleton = await call_tool(mcp_client, "anim_cmd", {
        "command": "get_skeleton_info",
        "params": {"asset_path": skeleton_path, "socket_limit": 20},
    })
    assert sequence["asset_type"] == "AnimSequence" and "fingerprint" in sequence
    assert montage["asset_type"] == "AnimMontage" and "fingerprint" in montage
    assert skeleton["asset_type"] == "Skeleton" and "fingerprint" in skeleton

    curve_name = _uniq("MCPScenario_Curve")
    curve = await call_tool(mcp_client, "anim_cmd", {
        "command": "add_curve",
        "params": {
            "asset_path": sequence_path,
            "curve_name": curve_name,
            "expected_fingerprint": sequence["fingerprint"],
            "dry_run": True,
            "save": False,
        },
    })
    assert curve["changed"] is True and curve["saved"] is False
    assert curve["dirty_after"] == curve["dirty_before"]

    montage_length = float(montage.get("length_seconds", 0.0))
    section_name = _uniq("MCPScenario_Section")
    section = await call_tool(mcp_client, "anim_cmd", {
        "command": "add_montage_section",
        "params": {
            "asset_path": montage_path,
            "name": section_name,
            "start_time": min(0.1, max(0.0, montage_length * 0.5)),
            "expected_fingerprint": montage["fingerprint"],
            "dry_run": True,
            "save": False,
        },
    })
    assert section["changed"] is True and section["saved"] is False
    assert section["dirty_after"] == section["dirty_before"]

    bones = skeleton.get("bones", {}).get("items", [])
    if not bones:
        pytest.skip("No reference skeleton bones available for socket dry-run")
    socket_name = _uniq("MCPScenario_Socket")
    socket = await call_tool(mcp_client, "anim_cmd", {
        "command": "add_socket",
        "params": {
            "asset_path": skeleton_path,
            "socket_name": socket_name,
            "bone_name": bones[0]["name"],
            "location": {"x": 1.0, "y": 2.0, "z": 3.0},
            "expected_fingerprint": skeleton["fingerprint"],
            "dry_run": True,
            "save": False,
        },
    })
    assert socket["changed"] is True and socket["saved"] is False
    assert socket["dirty_after"] == socket["dirty_before"]

    sequence_after = await call_tool(mcp_client, "anim_cmd", {
        "command": "get_sequence_info",
        "params": {"asset_path": sequence_path, "curve_name": curve_name, "curve_key_limit": 1},
    })
    montage_after = await call_tool(mcp_client, "anim_cmd", {
        "command": "get_montage_info",
        "params": {"asset_path": montage_path, "section_limit": 20},
    })
    skeleton_after = await call_tool(mcp_client, "anim_cmd", {
        "command": "get_skeleton_info",
        "params": {"asset_path": skeleton_path, "socket_limit": 20},
    })
    assert sequence_after["fingerprint"] == sequence["fingerprint"]
    assert montage_after["fingerprint"] == montage["fingerprint"]
    assert skeleton_after["fingerprint"] == skeleton["fingerprint"]
    assert sequence_after.get("curves", {}).get("count", 0) == 0
    assert all(item.get("name") != section_name for item in montage_after.get("sections", {}).get("items", []))
    assert all(item.get("socket_name") != socket_name for item in skeleton_after.get("sockets", {}).get("items", []))
