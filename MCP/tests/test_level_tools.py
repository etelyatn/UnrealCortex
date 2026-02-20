"""Unit tests for CortexLevel MCP tools."""

import sys
from pathlib import Path

import pytest

# Add tools/ to path for imports (matching existing test patterns)
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from level.composites import _build_level_batch_commands, _validate_level_batch_spec


class TestLegacySceneCompositeMigrated:
    """Regression tests confirming level_batch covers legacy scene-composite cases."""

    def test_empty_operations_valid(self):
        _validate_level_batch_spec([])

    def test_spawn_missing_class_raises(self):
        with pytest.raises(ValueError, match="missing 'class'"):
            _validate_level_batch_spec([{"op": "spawn", "id": "a"}])

    def test_duplicate_ids_raise(self):
        with pytest.raises(ValueError, match="Duplicate id"):
            _validate_level_batch_spec(
                [
                    {"op": "spawn", "id": "a", "class": "PointLight"},
                    {"op": "spawn", "id": "a", "class": "SpotLight"},
                ]
            )

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


class TestBatchCommandGenerationMigrated:
    """Regression tests confirming spawn batch command generation works as before."""

    def test_simple_spawn(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "spawn", "class": "PointLight", "location": [0, 0, 100]}],
            save=False,
        )
        assert len(commands) == 1
        assert commands[0]["command"] == "level.spawn_actor"
        assert commands[0]["params"]["class"] == "PointLight"

    def test_spawn_with_folder_and_tags(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "spawn", "id": "a", "class": "PointLight", "folder": "Lights", "tags": ["Interior"]}],
            save=False,
        )
        assert len(commands) == 3
        assert commands[1]["command"] == "level.set_folder"
        assert commands[2]["command"] == "level.set_tags"

    def test_actor_level_property(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "spawn", "id": "a", "class": "PointLight", "properties": {"bHidden": True}}],
            save=False,
        )
        prop_cmds = [c for c in commands if c["command"] == "level.set_actor_property"]
        assert len(prop_cmds) == 1
        assert prop_cmds[0]["params"]["property"] == "bHidden"

    def test_component_property(self):
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

    def test_attachment_wiring(self):
        commands, _, _ = _build_level_batch_commands(
            [
                {"op": "spawn", "id": "parent_actor", "class": "StaticMeshActor"},
                {"op": "spawn", "id": "child_actor", "class": "PointLight"},
                {"op": "attach", "actor": "$ops[child_actor].name", "parent": "$ops[parent_actor].name"},
            ],
            save=False,
        )
        attach_cmd = [c for c in commands if c["command"] == "level.attach_actor"]
        assert len(attach_cmd) == 1
        assert "$steps[0]" in attach_cmd[0]["params"]["parent"]
        assert "$steps[1]" in attach_cmd[0]["params"]["actor"]

    def test_save_appended(self):
        commands, _, _ = _build_level_batch_commands(
            [{"op": "spawn", "id": "a", "class": "PointLight"}],
            save=True,
        )
        assert commands[-1]["command"] == "level.save_level"
