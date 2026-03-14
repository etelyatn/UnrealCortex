"""Shared pytest path setup for legacy tool-module imports."""

from __future__ import annotations

import sys
import uuid
from pathlib import Path

import pytest

from cortex_mcp.tcp_client import UEConnection


_TESTS_DIR = Path(__file__).resolve().parent
_MCP_ROOT = _TESTS_DIR.parent
_SRC_DIR = _MCP_ROOT / "src"
_TOOLS_DIR = _SRC_DIR / "tools"
_EDITOR_TOOLS_DIR = _TOOLS_DIR / "editor"

for path in (_SRC_DIR, _TOOLS_DIR, _EDITOR_TOOLS_DIR):
    path_str = str(path)
    if path_str not in sys.path:
        sys.path.insert(0, path_str)


def _uniq(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex[:8]}"


@pytest.fixture(scope="session")
def tcp_connection():
    conn = UEConnection()
    conn.connect()
    try:
        yield conn
    finally:
        conn.disconnect()


@pytest.fixture
def cleanup_assets(tcp_connection):
    created: list[tuple[str, str]] = []
    try:
        yield created
    finally:
        for asset_type, asset_path in reversed(created):
            try:
                if asset_type == "material":
                    tcp_connection.send_command("material.delete_material", {"asset_path": asset_path})
                elif asset_type == "material_instance":
                    tcp_connection.send_command("material.delete_instance", {"asset_path": asset_path})
                else:
                    tcp_connection.send_command("bp.delete", {"asset_path": asset_path})
            except Exception:
                pass


@pytest.fixture
def blueprint_for_test(tcp_connection, cleanup_assets):
    name = _uniq("BP_E2E")
    resp = tcp_connection.send_command(
        "bp.create",
        {"name": name, "path": "/Game/Temp/CortexMCPTest", "type": "Actor"},
    )
    asset_path = resp["data"]["asset_path"]
    cleanup_assets.append(("blueprint", asset_path))
    return asset_path


@pytest.fixture
def widget_bp_for_test(tcp_connection, cleanup_assets):
    name = _uniq("WBP_E2E")
    resp = tcp_connection.send_command(
        "bp.create",
        {"name": name, "path": "/Game/Temp/CortexMCPTest", "type": "Widget"},
    )
    asset_path = resp["data"]["asset_path"]
    cleanup_assets.append(("blueprint", asset_path))
    return asset_path


@pytest.fixture
def cleanup_actors(tcp_connection):
    created: list[str] = []
    try:
        yield created
    finally:
        for actor_name in reversed(created):
            try:
                tcp_connection.send_command("level.delete_actor", {"actor": actor_name})
            except Exception:
                pass


@pytest.fixture
def actors_for_test(tcp_connection, cleanup_actors):
    actors = {}
    specs = {
        "light": {"class_name": "PointLight", "label": _uniq("CortexLight")},
        "camera": {"class_name": "CameraActor", "label": _uniq("CortexCamera")},
        "mesh": {"class_name": "StaticMeshActor", "label": _uniq("CortexMesh")},
    }

    for key, spec in specs.items():
        resp = tcp_connection.send_command(
            "level.spawn_actor",
            {"class_name": spec["class_name"], "label": spec["label"]},
        )
        actor_name = resp["data"]["name"]
        cleanup_actors.append(actor_name)
        actors[key] = actor_name

    return actors
