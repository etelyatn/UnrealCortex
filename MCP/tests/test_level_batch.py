"""Unit tests for level_batch composite tool helper functions."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock

import pytest

tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from level.composites import (
    _build_level_batch_commands,
    _validate_level_batch_spec,
    register_level_composite_tools,
)


class TestValidateLevelBatchSpec:
    """Test _validate_level_batch_spec() input validation."""

    def test_empty_operations_valid(self):
        _validate_level_batch_spec([])

    def test_unknown_op_type_raises(self):
        with pytest.raises(ValueError, match="unknown op type"):
            _validate_level_batch_spec([{"op": "teleport", "actor": "X"}])

    def test_missing_op_field_raises(self):
        with pytest.raises(ValueError, match="missing 'op'"):
            _validate_level_batch_spec([{"class": "PointLight"}])

    def test_spawn_missing_class_raises(self):
        with pytest.raises(ValueError, match="missing 'class'"):
            _validate_level_batch_spec([{"op": "spawn", "id": "a"}])

    def test_modify_missing_actor_raises(self):
        with pytest.raises(ValueError, match="missing 'actor'"):
            _validate_level_batch_spec([{"op": "modify", "folder": "Foo"}])

    def test_delete_missing_actor_raises(self):
        with pytest.raises(ValueError, match="missing 'actor'"):
            _validate_level_batch_spec([{"op": "delete"}])

    def test_attach_missing_parent_raises(self):
        with pytest.raises(ValueError, match="missing 'parent'"):
            _validate_level_batch_spec([{"op": "attach", "actor": "X"}])

    def test_duplicate_id_raises(self):
        with pytest.raises(ValueError, match="Duplicate id"):
            _validate_level_batch_spec(
                [
                    {"op": "spawn", "id": "a", "class": "PointLight"},
                    {"op": "spawn", "id": "a", "class": "SpotLight"},
                ]
            )

    def test_forward_ref_raises(self):
        with pytest.raises(ValueError, match="forward"):
            _validate_level_batch_spec(
                [
                    {"op": "attach", "actor": "$ops[notyet].name", "parent": "ExistingActor"},
                    {"op": "spawn", "id": "notyet", "class": "PointLight"},
                ]
            )

    def test_existing_actor_parent_in_attach_valid(self):
        """Attach to pre-existing actor by label - not a batch-local id - must not raise."""
        _validate_level_batch_spec(
            [
                {"op": "spawn", "id": "child", "class": "StaticMeshActor"},
                {"op": "attach", "actor": "$ops[child].name", "parent": "VehicleBody"},
            ]
        )

    def test_unsupported_ref_accessor_raises(self):
        with pytest.raises(ValueError, match="Only.*\\.name.*supported"):
            _validate_level_batch_spec(
                [
                    {"op": "spawn", "id": "a", "class": "PointLight"},
                    {"op": "modify", "actor": "$ops[a].label", "folder": "Foo"},
                ]
            )

    def test_modify_with_no_change_fields_raises(self):
        with pytest.raises(ValueError, match="no modification fields"):
            _validate_level_batch_spec([{"op": "modify", "actor": "Light_01"}])

    def test_empty_actor_string_raises(self):
        with pytest.raises(ValueError, match="empty 'actor'"):
            _validate_level_batch_spec([{"op": "modify", "actor": "", "folder": "Foo"}])

    def test_ref_injection_in_properties_raises(self):
        with pytest.raises(ValueError, match=r"\$steps\["):
            _validate_level_batch_spec(
                [
                    {
                        "op": "spawn",
                        "id": "a",
                        "class": "PointLight",
                        "properties": {"bHidden": "$steps[0].data.name"},
                    }
                ]
            )

    def test_valid_full_spec(self):
        _validate_level_batch_spec(
            [
                {
                    "op": "spawn",
                    "id": "light",
                    "class": "PointLight",
                    "label": "Fill",
                    "folder": "Lighting",
                    "tags": ["indoor"],
                    "properties": {"PointLightComponent0.Intensity": 5000},
                },
                {
                    "op": "modify",
                    "actor": "ExistingMesh",
                    "folder": "Geometry",
                    "transform": {"location": [0, 0, 100]},
                },
                {"op": "delete", "actor": "OldActor"},
                {"op": "duplicate", "actor": "Wall_A", "id": "wall_b", "offset": [200, 0, 0]},
                {"op": "attach", "actor": "$ops[light].name", "parent": "$ops[wall_b].name"},
                {"op": "detach", "actor": "SomeChild"},
            ]
        )


class TestBuildLevelBatchCommands:
    """Test _build_level_batch_commands() TCP command translation."""

    def test_spawn_produces_spawn_command(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "spawn", "class": "PointLight", "location": [0, 0, 100]}],
            save=False,
        )
        assert len(commands) == 1
        assert commands[0]["command"] == "level.spawn_actor"
        assert commands[0]["params"]["class"] == "PointLight"
        assert commands[0]["params"]["location"] == [0, 0, 100]

    def test_spawn_with_folder_and_tags(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "spawn", "id": "a", "class": "PointLight", "folder": "Lights", "tags": ["interior"]}],
            save=False,
        )
        assert len(commands) == 3
        assert commands[0]["command"] == "level.spawn_actor"
        assert commands[1]["command"] == "level.set_folder"
        assert commands[1]["params"]["folder"] == "Lights"
        assert commands[2]["command"] == "level.set_tags"
        assert commands[2]["params"]["tags"] == ["interior"]

    def test_spawn_component_property(self):
        commands, _, _ = _build_level_batch_commands(
            [
                {
                    "op": "spawn",
                    "id": "a",
                    "class": "PointLight",
                    "properties": {"PointLightComponent0.Intensity": 8000},
                }
            ],
            save=False,
        )
        prop_cmds = [c for c in commands if c["command"] == "level.set_component_property"]
        assert len(prop_cmds) == 1
        assert prop_cmds[0]["params"]["component"] == "PointLightComponent0"
        assert prop_cmds[0]["params"]["property"] == "Intensity"
        assert prop_cmds[0]["params"]["value"] == 8000

    def test_spawn_actor_property(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "spawn", "id": "a", "class": "PointLight", "properties": {"bHidden": True}}],
            save=False,
        )
        prop_cmds = [c for c in commands if c["command"] == "level.set_actor_property"]
        assert len(prop_cmds) == 1
        assert prop_cmds[0]["params"]["property"] == "bHidden"

    def test_modify_transform(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "modify", "actor": "Light_01", "transform": {"location": [100, 0, 300]}}],
            save=False,
        )
        assert len(commands) == 1
        assert commands[0]["command"] == "level.set_transform"
        assert commands[0]["params"]["actor"] == "Light_01"
        assert commands[0]["params"]["location"] == [100, 0, 300]

    def test_modify_label(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "modify", "actor": "OldName", "label": "NewName"}],
            save=False,
        )
        assert any(c["command"] == "level.rename_actor" for c in commands)
        rename = next(c for c in commands if c["command"] == "level.rename_actor")
        assert rename["params"]["label"] == "NewName"

    def test_modify_data_layer(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "modify", "actor": "Actor_01", "data_layer": "Gameplay"}],
            save=False,
        )
        assert any(c["command"] == "level.set_data_layer" for c in commands)
        dl = next(c for c in commands if c["command"] == "level.set_data_layer")
        assert dl["params"]["data_layer"] == "Gameplay"

    def test_modify_properties(self):
        commands, _, _ = _build_level_batch_commands(
            [
                {
                    "op": "modify",
                    "actor": "Light_01",
                    "properties": {"PointLightComponent0.Intensity": 5000},
                }
            ],
            save=False,
        )
        prop_cmds = [c for c in commands if c["command"] == "level.set_component_property"]
        assert len(prop_cmds) == 1
        assert prop_cmds[0]["params"]["component"] == "PointLightComponent0"
        assert prop_cmds[0]["params"]["property"] == "Intensity"
        assert prop_cmds[0]["params"]["value"] == 5000

    def test_modify_with_ops_ref(self):
        commands, _, _ = _build_level_batch_commands(
            [
                {"op": "spawn", "id": "light", "class": "PointLight"},
                {"op": "modify", "actor": "$ops[light].name", "folder": "Lighting"},
            ],
            save=False,
        )
        modify_cmd = next(c for c in commands if c["command"] == "level.set_folder")
        assert "$steps[0].data.name" == modify_cmd["params"]["actor"]

    def test_duplicate_with_ops_ref(self):
        commands, _, _ = _build_level_batch_commands(
            [
                {"op": "spawn", "id": "original", "class": "StaticMeshActor"},
                {"op": "duplicate", "id": "copy", "actor": "$ops[original].name", "offset": [200, 0, 0]},
            ],
            save=False,
        )
        dup_cmd = next(c for c in commands if c["command"] == "level.duplicate_actor")
        assert "$steps[0].data.name" == dup_cmd["params"]["actor"]

    def test_delete_produces_delete_command(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "delete", "actor": "OldActor"}],
            save=False,
        )
        assert len(commands) == 1
        assert commands[0]["command"] == "level.delete_actor"
        assert commands[0]["params"]["actor"] == "OldActor"

    def test_duplicate_produces_duplicate_command(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "duplicate", "id": "copy", "actor": "Wall_A", "offset": [200, 0, 0]}],
            save=False,
        )
        assert len(commands) == 1
        assert commands[0]["command"] == "level.duplicate_actor"
        assert commands[0]["params"]["offset"] == [200, 0, 0]

    def test_attach_with_ops_refs(self):
        commands, _, _ = _build_level_batch_commands(
            [
                {"op": "spawn", "id": "body", "class": "StaticMeshActor"},
                {"op": "spawn", "id": "turret", "class": "StaticMeshActor"},
                {"op": "attach", "actor": "$ops[turret].name", "parent": "$ops[body].name"},
            ],
            save=False,
        )
        attach = next(c for c in commands if c["command"] == "level.attach_actor")
        assert "$steps[0]" in attach["params"]["parent"]
        assert "$steps[1]" in attach["params"]["actor"]

    def test_attach_with_existing_actor_parent(self):
        """parent can be a plain label for a pre-existing actor - no $ops ref required."""
        commands, _, _ = _build_level_batch_commands(
            [
                {"op": "spawn", "id": "child", "class": "PointLight"},
                {"op": "attach", "actor": "$ops[child].name", "parent": "VehicleBody"},
            ],
            save=False,
        )
        attach = next(c for c in commands if c["command"] == "level.attach_actor")
        assert attach["params"]["parent"] == "VehicleBody"

    def test_detach_produces_detach_command(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "detach", "actor": "Child_01"}],
            save=False,
        )
        assert len(commands) == 1
        assert commands[0]["command"] == "level.detach_actor"

    def test_save_appended_when_true(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "spawn", "id": "a", "class": "PointLight"}],
            save=True,
        )
        assert commands[-1]["command"] == "level.save_level"

    def test_save_not_appended_when_false(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "spawn", "id": "a", "class": "PointLight"}],
            save=False,
        )
        assert not any(c["command"] == "level.save_level" for c in commands)

    def test_step_to_op_info_maps_first_step(self):
        """step_to_op_info keys align with the first command index of each op."""
        _, _, step_to_op_info = _build_level_batch_commands(
            [
                {"op": "spawn", "id": "a", "class": "PointLight", "folder": "Lighting"},
                {"op": "modify", "actor": "Existing", "folder": "Geometry"},
            ],
            save=False,
        )
        # spawn "a" starts at step 0, modify starts after spawn + set_folder = step 2
        assert 0 in step_to_op_info
        assert step_to_op_info[0]["op_id"] == "a"
        assert 2 in step_to_op_info
        assert step_to_op_info[2]["op_id"] is None

    def test_id_to_step_maps_spawn_id(self):
        _, id_to_step, _ = _build_level_batch_commands(
            [{"op": "spawn", "id": "mylight", "class": "PointLight"}],
            save=False,
        )
        assert "mylight" in id_to_step
        assert id_to_step["mylight"] == 0


class TestLevelBatchTool:
    """Test level_batch MCP tool with mocked UE connection."""

    def _get_tool(self, mock_connection):
        """Register tools with a mock MCP and return the captured level_batch function."""
        captured = {}

        def capture_tool():
            def decorator(fn):
                captured[fn.__name__] = fn
                return fn

            return decorator

        mock_mcp = MagicMock()
        mock_mcp.tool = capture_tool
        register_level_composite_tools(mock_mcp, mock_connection)
        return captured["level_batch"]

    def test_invalid_spec_returns_error(self):
        conn = MagicMock()
        tool = self._get_tool(conn)
        result = json.loads(tool(operations=[{"op": "spawn"}]))  # missing class
        assert result["success"] is False
        assert "Invalid spec" in result["error"]
        conn.send_command.assert_not_called()

    def test_success_response_shape(self):
        conn = MagicMock()
        conn.send_command.return_value = {
            "data": {
                "results": [
                    {
                        "success": True,
                        "index": 0,
                        "command": "level.spawn_actor",
                        "data": {"name": "PointLight_0"},
                        "timing_ms": 10,
                    },
                ],
                "total_timing_ms": 10,
            }
        }
        tool = self._get_tool(conn)
        result = json.loads(
            tool(
                operations=[{"op": "spawn", "class": "PointLight"}],
                stop_on_error=False,
                save=False,
            )
        )
        assert result["success"] is True
        assert result["actor_count"] == 1
        assert "PointLight_0" in result["spawned_actors"]
        assert result["completed_steps"] == result["total_steps"]

    def test_partial_failure_response_includes_op_id(self):
        conn = MagicMock()
        conn.send_command.return_value = {
            "data": {
                "results": [
                    {
                        "success": True,
                        "index": 0,
                        "command": "level.spawn_actor",
                        "data": {"name": "StaticMeshActor_0"},
                        "timing_ms": 5,
                    },
                    {
                        "success": False,
                        "index": 1,
                        "command": "level.set_folder",
                        "error_message": "ActorNotFound",
                        "error_code": "ActorNotFound",
                        "timing_ms": 2,
                    },
                ],
                "total_timing_ms": 7,
            }
        }
        tool = self._get_tool(conn)
        result = json.loads(
            tool(
                operations=[{"op": "spawn", "id": "wall", "class": "StaticMeshActor", "folder": "Geometry"}],
                stop_on_error=False,
                save=False,
            )
        )
        assert result["success"] is False
        assert len(result["failed_steps"]) == 1
        assert result["failed_steps"][0]["op_id"] == "wall"

    def test_stop_on_error_passed_to_batch(self):
        conn = MagicMock()
        conn.send_command.return_value = {"data": {"results": [], "total_timing_ms": 0}}
        tool = self._get_tool(conn)
        tool(operations=[], stop_on_error=True, save=False)
        batch_params = conn.send_command.call_args[0][1]
        assert batch_params["stop_on_error"] is True

    def test_connection_error_returns_error(self):
        conn = MagicMock()
        conn.send_command.side_effect = ConnectionError("refused")
        tool = self._get_tool(conn)
        result = json.loads(
            tool(
                operations=[{"op": "spawn", "class": "PointLight"}],
                save=False,
            )
        )
        assert result["success"] is False
        assert "Connection error" in result["error"]
