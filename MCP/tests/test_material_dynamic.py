"""Unit tests for dynamic material instance MCP tools (mock TCP, no editor required)."""

import json
import os
import sys
import unittest
from unittest.mock import MagicMock

from cortex_mcp.tcp_client import UEConnection

# Add MCP root to path so 'from tools.material.* import' works.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))


class TestDynamicMaterialTools(unittest.TestCase):
    """Test dynamic material tool registration and parameter passing."""

    def setUp(self):
        self.connection = MagicMock(spec=UEConnection)
        self.mcp = MagicMock()
        self.registered_tools = {}

        def mock_tool():
            def decorator(func):
                self.registered_tools[func.__name__] = func
                return func

            return decorator

        self.mcp.tool = mock_tool

        from tools.material.dynamic import register_material_dynamic_tools

        register_material_dynamic_tools(self.mcp, self.connection)

    def test_all_tools_registered(self):
        expected = {
            "list_dynamic_instances",
            "get_dynamic_instance",
            "create_dynamic_instance",
            "destroy_dynamic_instance",
            "set_dynamic_parameter",
            "get_dynamic_parameter",
            "list_dynamic_parameters",
            "set_dynamic_parameters",
            "reset_dynamic_parameter",
        }
        self.assertEqual(set(self.registered_tools.keys()), expected)

    def test_list_dynamic_instances_params(self):
        self.connection.send_command.return_value = {"data": {"components": []}}

        self.registered_tools["list_dynamic_instances"](actor_path="MyActor")

        self.connection.send_command.assert_called_once_with(
            "material.list_dynamic_instances",
            {"actor_path": "MyActor"},
        )

    def test_create_dynamic_instance_with_defaults(self):
        self.connection.send_command.return_value = {"data": {}}

        self.registered_tools["create_dynamic_instance"](actor_path="MyActor")

        call_args = self.connection.send_command.call_args
        params = call_args[0][1]
        self.assertEqual(params["actor_path"], "MyActor")
        self.assertEqual(params["slot_index"], 0)
        self.assertNotIn("component_name", params)
        self.assertNotIn("source_material", params)

    def test_create_dynamic_instance_with_all_params(self):
        self.connection.send_command.return_value = {"data": {}}

        self.registered_tools["create_dynamic_instance"](
            actor_path="MyActor",
            component_name="StaticMeshComponent0",
            slot_index=1,
            source_material="/Game/Materials/M_Test",
            parameters=[{"name": "Roughness", "type": "scalar", "value": 0.5}],
        )

        call_args = self.connection.send_command.call_args
        params = call_args[0][1]
        self.assertEqual(params["component_name"], "StaticMeshComponent0")
        self.assertEqual(params["slot_index"], 1)
        self.assertEqual(params["source_material"], "/Game/Materials/M_Test")
        self.assertEqual(len(params["parameters"]), 1)

    def test_set_dynamic_parameter_params(self):
        self.connection.send_command.return_value = {"data": {}}

        self.registered_tools["set_dynamic_parameter"](
            actor_path="MyActor",
            name="Roughness",
            parameter_type="scalar",
            value=0.8,
        )

        call_args = self.connection.send_command.call_args
        self.assertEqual(call_args[0][0], "material.set_dynamic_parameter")
        params = call_args[0][1]
        self.assertEqual(params["name"], "Roughness")
        self.assertEqual(params["type"], "scalar")
        self.assertEqual(params["value"], 0.8)

    def test_set_dynamic_parameters_batch(self):
        self.connection.send_command.return_value = {"data": {}}

        batch = [
            {"name": "Roughness", "type": "scalar", "value": 0.8},
            {"name": "BaseColor", "type": "vector", "value": [1, 0, 0, 1]},
        ]
        self.registered_tools["set_dynamic_parameters"](actor_path="MyActor", parameters=batch)

        call_args = self.connection.send_command.call_args
        params = call_args[0][1]
        self.assertEqual(len(params["parameters"]), 2)

    def test_get_dynamic_instance_params(self):
        self.connection.send_command.return_value = {"data": {"parameters": {}}}

        self.registered_tools["get_dynamic_instance"](
            actor_path="MyActor",
            component_name="Mesh0",
            slot_index=2,
        )

        call_args = self.connection.send_command.call_args
        self.assertEqual(call_args[0][0], "material.get_dynamic_instance")
        params = call_args[0][1]
        self.assertEqual(params["actor_path"], "MyActor")
        self.assertEqual(params["component_name"], "Mesh0")
        self.assertEqual(params["slot_index"], 2)

    def test_destroy_dynamic_instance_params(self):
        self.connection.send_command.return_value = {"data": {}}

        self.registered_tools["destroy_dynamic_instance"](actor_path="MyActor", slot_index=1)

        call_args = self.connection.send_command.call_args
        self.assertEqual(call_args[0][0], "material.destroy_dynamic_instance")

    def test_get_dynamic_parameter_params(self):
        self.connection.send_command.return_value = {"data": {}}

        self.registered_tools["get_dynamic_parameter"](actor_path="MyActor", name="BaseColor")

        call_args = self.connection.send_command.call_args
        self.assertEqual(call_args[0][0], "material.get_dynamic_parameter")
        params = call_args[0][1]
        self.assertEqual(params["name"], "BaseColor")

    def test_list_dynamic_parameters_params(self):
        self.connection.send_command.return_value = {"data": {"parameters": {}}}

        self.registered_tools["list_dynamic_parameters"](actor_path="MyActor")

        call_args = self.connection.send_command.call_args
        self.assertEqual(call_args[0][0], "material.list_dynamic_parameters")

    def test_reset_dynamic_parameter_params(self):
        self.connection.send_command.return_value = {"data": {}}

        self.registered_tools["reset_dynamic_parameter"](actor_path="MyActor", name="Roughness")

        call_args = self.connection.send_command.call_args
        self.assertEqual(call_args[0][0], "material.reset_dynamic_parameter")
        params = call_args[0][1]
        self.assertEqual(params["name"], "Roughness")

    def test_error_handling(self):
        self.connection.send_command.side_effect = ConnectionError("No editor")

        result = self.registered_tools["list_dynamic_instances"](actor_path="MyActor")

        parsed = json.loads(result)
        self.assertIn("error", parsed)
        self.assertIn("No editor", parsed["error"])

    def test_error_handling_per_tool(self):
        self.connection.send_command.side_effect = ConnectionError("No editor")

        for tool_name in [
            "get_dynamic_instance",
            "create_dynamic_instance",
            "destroy_dynamic_instance",
            "set_dynamic_parameter",
        ]:
            kwargs = {"actor_path": "MyActor"}
            if tool_name == "set_dynamic_parameter":
                kwargs.update(name="X", parameter_type="scalar", value=0.5)

            result = self.registered_tools[tool_name](**kwargs)
            parsed = json.loads(result)
            self.assertIn("error", parsed, f"{tool_name} should return error JSON")


if __name__ == "__main__":
    unittest.main()
