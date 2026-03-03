"""E2E test: create material instance with parameters via composite tool.

Requires running UE editor with CortexMaterial domain registered.
Run: cd Plugins/UnrealCortex/MCP && uv run pytest tests/test_material_instance_composites_e2e.py -v
"""

import os
import sys
import uuid

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

from cortex_mcp.tcp_client import UEConnection
from material.composites import _build_instance_batch_commands


def _get_connection():
    """Get connection to UE editor, skip test if editor not running."""
    conn = UEConnection()
    if conn.port == 8742 and conn._pid is None:
        pytest.skip("No CortexPort-*.txt - UE editor not running")
    try:
        conn.connect()
    except Exception:
        pytest.skip("Cannot connect to UE editor")
    return conn


def _uniq(prefix: str) -> str:
    """Generate unique name with GUID suffix."""
    return f"{prefix}_{uuid.uuid4().hex[:8]}"


def _create_test_parent(conn, name):
    """Create a parent material with ScalarParameter and VectorParameter."""
    commands = [
        {"command": "material.create_material", "params": {
            "name": name, "asset_path": "/Game/Temp/CortexMCPTest/",
        }},
        {"command": "material.add_node", "params": {
            "asset_path": "$steps[0].data.asset_path",
            "expression_class": "MaterialExpressionScalarParameter",
        }},
        {"command": "material.set_node_property", "params": {
            "asset_path": "$steps[0].data.asset_path",
            "node_id": "$steps[1].data.node_id",
            "property_name": "ParameterName",
            "value": "Roughness",
        }},
        {"command": "material.add_node", "params": {
            "asset_path": "$steps[0].data.asset_path",
            "expression_class": "MaterialExpressionVectorParameter",
        }},
        {"command": "material.set_node_property", "params": {
            "asset_path": "$steps[0].data.asset_path",
            "node_id": "$steps[3].data.node_id",
            "property_name": "ParameterName",
            "value": "Color",
        }},
    ]

    result = conn.send_command("batch", {
        "stop_on_error": True,
        "commands": commands,
    }, timeout=60)

    assert result.get("success") is True
    return result["data"]["results"][0]["data"]["asset_path"]


@pytest.mark.e2e
class TestMaterialInstanceCompositeE2E:
    """E2E tests for material instance composites."""

    def test_create_instance_with_params(self):
        """Create instance with scalar and vector parameter overrides."""
        conn = _get_connection()
        parent_name = _uniq("M_E2E_InstParent")
        parent_path = _create_test_parent(conn, parent_name)
        instance_name = _uniq("MI_E2E_WithParams")

        commands = _build_instance_batch_commands(
            name=instance_name,
            path="/Game/Temp/CortexMCPTest/",
            parent=parent_path,
            parameters=[
                {"name": "Roughness", "type": "scalar", "value": 0.8},
                {"name": "Color", "type": "vector", "value": {"R": 1, "G": 0, "B": 0, "A": 1}},
            ],
        )

        instance_path = None
        try:
            result = conn.send_command("batch", {
                "stop_on_error": True,
                "commands": commands,
            }, timeout=60)

            assert result.get("success") is True
            results = result["data"]["results"]
            for entry in results:
                assert entry.get("success") is True, (
                    f"Step {entry.get('index')} failed: {entry.get('error_message')}"
                )

            instance_path = results[0]["data"]["asset_path"]
            info = conn.send_command("material.get_instance", {"asset_path": instance_path})
            assert info.get("success") is True
            overrides = info.get("data", {}).get("overrides", {})
            scalar_names = [p["name"] for p in overrides.get("scalar", [])]
            vector_names = [p["name"] for p in overrides.get("vector", [])]
            assert "Roughness" in scalar_names
            assert "Color" in vector_names
        finally:
            if instance_path:
                try:
                    conn.send_command("material.delete_instance", {"asset_path": instance_path})
                except Exception:
                    pass
            try:
                conn.send_command("material.delete_material", {"asset_path": parent_path})
            except Exception:
                pass

    def test_create_instance_no_params(self):
        """Create instance with no parameter overrides."""
        conn = _get_connection()
        parent_name = _uniq("M_E2E_InstNoParam")
        parent_path = _create_test_parent(conn, parent_name)
        instance_name = _uniq("MI_E2E_NoParams")

        commands = _build_instance_batch_commands(
            name=instance_name,
            path="/Game/Temp/CortexMCPTest/",
            parent=parent_path,
            parameters=[],
        )

        instance_path = None
        try:
            result = conn.send_command("batch", {
                "stop_on_error": True,
                "commands": commands,
            }, timeout=60)

            assert result.get("success") is True
            instance_path = result["data"]["results"][0]["data"]["asset_path"]
            info = conn.send_command("material.get_instance", {"asset_path": instance_path})
            assert info.get("success") is True
        finally:
            if instance_path:
                try:
                    conn.send_command("material.delete_instance", {"asset_path": instance_path})
                except Exception:
                    pass
            try:
                conn.send_command("material.delete_material", {"asset_path": parent_path})
            except Exception:
                pass
