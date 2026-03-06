"""Unit tests for Blueprint class defaults MCP tools."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock

# Add tools directory to path for imports
TOOLS_DIR = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from blueprint.class_defaults import register_blueprint_class_defaults_tools


class MockMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator


def _register_tools(connection):
    mcp = MockMCP()
    register_blueprint_class_defaults_tools(mcp, connection)
    return mcp.tools


def _fake_success_data():
    return {
        "data": {
            "blueprint_path": "/Game/Test/BP_Test",
            "properties": {},
        }
    }


class TestGetClassDefaults:
    def test_discovery_mode_omits_properties_param(self):
        connection = MagicMock()
        connection.send_command.return_value = _fake_success_data()

        tools = _register_tools(connection)
        result_json = tools["get_class_defaults"]("/Game/Test/BP_Test")

        parsed = json.loads(result_json)
        assert parsed["blueprint_path"] == "/Game/Test/BP_Test"
        connection.send_command.assert_called_once_with(
            "bp.get_class_defaults",
            {"asset_path": "/Game/Test/BP_Test"},
        )

    def test_specific_properties_are_forwarded(self):
        connection = MagicMock()
        connection.send_command.return_value = _fake_success_data()

        tools = _register_tools(connection)
        tools["get_class_defaults"](
            "/Game/Test/BP_Test",
            ["bReplicates", "NetCullDistanceSquared"],
        )

        connection.send_command.assert_called_once_with(
            "bp.get_class_defaults",
            {
                "asset_path": "/Game/Test/BP_Test",
                "properties": ["bReplicates", "NetCullDistanceSquared"],
            },
        )


class TestSetClassDefaults:
    def test_default_compile_and_save_true(self):
        connection = MagicMock()
        connection.send_command.return_value = _fake_success_data()

        tools = _register_tools(connection)
        tools["set_class_defaults"](
            "/Game/Test/BP_Test",
            {"bReplicates": True},
        )

        connection.send_command.assert_called_once_with(
            "bp.set_class_defaults",
            {
                "asset_path": "/Game/Test/BP_Test",
                "properties": {"bReplicates": True},
                "compile": True,
                "save": True,
            },
        )

    def test_compile_and_save_false_are_forwarded(self):
        connection = MagicMock()
        connection.send_command.return_value = _fake_success_data()

        tools = _register_tools(connection)
        tools["set_class_defaults"](
            "/Game/Test/BP_Test",
            {"MaxHealth": 200.0},
            compile=False,
            save=False,
        )

        connection.send_command.assert_called_once_with(
            "bp.set_class_defaults",
            {
                "asset_path": "/Game/Test/BP_Test",
                "properties": {"MaxHealth": 200.0},
                "compile": False,
                "save": False,
            },
        )

    def test_object_reference_values_are_passed_as_strings(self):
        connection = MagicMock()
        connection.send_command.return_value = _fake_success_data()

        tools = _register_tools(connection)
        tools["set_class_defaults"](
            "/Game/Sim/Blueprints/BP_SimCharacter",
            {
                "MoveAction": "/Game/Sim/Input/IA_Move",
                "LookAction": "/Game/Sim/Input/IA_Look",
            },
        )

        call = connection.send_command.call_args
        props = call.args[1]["properties"]
        assert props["MoveAction"] == "/Game/Sim/Input/IA_Move"
        assert props["LookAction"] == "/Game/Sim/Input/IA_Look"
