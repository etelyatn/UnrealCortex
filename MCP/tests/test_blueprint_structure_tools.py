"""Unit tests for Blueprint structure MCP tools."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock

tools_dir = Path(__file__).parent.parent / "src" / "tools"
sys.path.insert(0, str(tools_dir))

from blueprint.structure import register_blueprint_structure_tools


class MockMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator


def register_tools(connection):
    mcp = MockMCP()
    register_blueprint_structure_tools(mcp, connection)
    return mcp.tools


def test_set_component_defaults_forwards_json_properties_and_write_options():
    connection = MagicMock()
    connection.send_command.return_value = {
        "data": {
            "component_name": "StaticMeshComponent0",
            "properties_set": 4,
            "partial_failure": False,
            "errors": [],
        }
    }
    tools = register_tools(connection)

    properties = {
        "StaticMesh": "/Engine/BasicShapes/Cube.Cube",
        "OverrideMaterials[0]": "/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial",
        "RelativeLocation": {"X": 100, "Y": 0, "Z": 50},
        "bVisible": False,
    }
    result = tools["set_component_defaults"](
        asset_path="/Game/Blueprints/BP_Test",
        component_name="StaticMeshComponent0",
        properties=properties,
        compile=True,
        save=False,
        expected_fingerprint={"hash": "abc123"},
    )

    connection.send_command.assert_called_once_with(
        "blueprint.set_component_defaults",
        {
            "asset_path": "/Game/Blueprints/BP_Test",
            "component_name": "StaticMeshComponent0",
            "properties": properties,
            "compile": True,
            "save": False,
            "expected_fingerprint": {"hash": "abc123"},
        },
    )
    payload = json.loads(result)
    assert payload["properties_set"] == 4
    assert payload["partial_failure"] is False
    assert payload["errors"] == []


def test_set_component_defaults_forwards_batch_items_with_compile_save():
    connection = MagicMock()
    connection.send_command.return_value = {
        "data": {
            "status": "committed",
            "per_item": [
                {
                    "target": "/Game/Blueprints/BP_Test",
                    "success": True,
                    "data": {"partial_failure": True, "errors": ["bad value"]},
                }
            ],
        }
    }
    tools = register_tools(connection)

    items = [
        {
            "target": "/Game/Blueprints/BP_Test",
            "component_name": "StaticMeshComponent0",
            "properties": {"RelativeLocation": {"X": 1, "Y": 2, "Z": 3}},
            "compile": False,
            "save": True,
        }
    ]
    tools["set_component_defaults"](items=items)

    connection.send_command.assert_called_once_with(
        "blueprint.set_component_defaults",
        {"items": items},
    )
