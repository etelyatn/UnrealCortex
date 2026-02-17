"""Unit tests for CortexLevel MCP tools."""

import sys
from pathlib import Path

import pytest

# Add tools/ to path for imports (matching existing test patterns)
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from level.composites import _validate_scene_spec, _build_batch_commands


class TestSceneCompositeValidation:
    """Test create_level_scene spec validation."""

    def test_validate_empty_actors(self):
        _validate_scene_spec([], None)

    def test_validate_missing_class(self):
        with pytest.raises(ValueError, match="missing 'class'"):
            _validate_scene_spec([{"id": "a"}], None)

    def test_validate_duplicate_ids(self):
        with pytest.raises(ValueError, match="Duplicate"):
            _validate_scene_spec([
                {"id": "a", "class": "PointLight"},
                {"id": "a", "class": "SpotLight"},
            ], None)

    def test_validate_bad_attachment(self):
        with pytest.raises(ValueError, match="Unknown attachment child"):
            _validate_scene_spec(
                [{"id": "a", "class": "PointLight"}],
                {"attachments": [{"child": "b", "parent": "a"}]},
            )

    def test_validate_ref_injection(self):
        with pytest.raises(ValueError, match="\\$steps\\["):
            _validate_scene_spec(
                [{"id": "a", "class": "PointLight", "properties": {"bHidden": "$steps[0].data.name"}}],
                None,
            )


class TestBatchCommandGeneration:
    """Test batch command building from scene specs."""

    def test_simple_spawn(self):
        commands = _build_batch_commands(
            [{"id": "light", "class": "PointLight", "location": [0, 0, 100]}],
            None,
            save=False,
        )
        assert len(commands) == 1
        assert commands[0]["command"] == "level.spawn_actor"
        assert commands[0]["params"]["class"] == "PointLight"

    def test_spawn_with_folder_and_tags(self):
        commands = _build_batch_commands(
            [{"id": "a", "class": "PointLight", "folder": "Lights", "tags": ["Interior"]}],
            None,
            save=False,
        )
        assert len(commands) == 3
        assert commands[1]["command"] == "level.set_folder"
        assert commands[2]["command"] == "level.set_tags"

    def test_actor_level_property(self):
        commands = _build_batch_commands(
            [{"id": "a", "class": "PointLight", "properties": {"bHidden": True}}],
            None,
            save=False,
        )
        prop_cmds = [c for c in commands if c["command"] == "level.set_actor_property"]
        assert len(prop_cmds) == 1
        assert prop_cmds[0]["params"]["property"] == "bHidden"

    def test_component_property(self):
        commands = _build_batch_commands(
            [{"id": "a", "class": "PointLight", "properties": {"PointLightComponent0.Intensity": 8000}}],
            None,
            save=False,
        )
        prop_cmds = [c for c in commands if c["command"] == "level.set_component_property"]
        assert len(prop_cmds) == 1
        assert prop_cmds[0]["params"]["component"] == "PointLightComponent0"
        assert prop_cmds[0]["params"]["property"] == "Intensity"

    def test_attachment_wiring(self):
        commands = _build_batch_commands(
            [
                {"id": "parent", "class": "StaticMeshActor"},
                {"id": "child", "class": "PointLight"},
            ],
            {"attachments": [{"child": "child", "parent": "parent"}]},
            save=False,
        )
        attach_cmd = [c for c in commands if c["command"] == "level.attach_actor"]
        assert len(attach_cmd) == 1
        assert "$steps[0]" in attach_cmd[0]["params"]["parent"]
        assert "$steps[1]" in attach_cmd[0]["params"]["actor"]

    def test_save_appended(self):
        commands = _build_batch_commands(
            [{"id": "a", "class": "PointLight"}],
            None,
            save=True,
        )
        assert commands[-1]["command"] == "level.save_level"
