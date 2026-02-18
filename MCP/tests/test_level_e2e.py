"""End-to-end TCP tests for the Level domain.

Requires a running Unreal Editor with CortexLevel domain registered.

Run:
    cd Plugins/UnrealCortex/MCP && uv run pytest tests/test_level_e2e.py -v
"""

import subprocess
import uuid
from pathlib import Path

import pytest


def _uniq(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex[:8]}"


# ================================================================
# Discovery (read-only, no fixtures)
# ================================================================


@pytest.mark.e2e
class TestLevelDiscovery:

    def test_list_actor_classes(self, tcp_connection):
        resp = tcp_connection.send_command("level.list_actor_classes", {})
        data = resp["data"]
        assert "classes" in data
        assert data.get("count", len(data["classes"])) > 0

    def test_list_actor_classes_filtered(self, tcp_connection):
        resp = tcp_connection.send_command(
            "level.list_actor_classes", {"category": "lights"},
        )
        data = resp["data"]
        assert "classes" in data
        assert len(data["classes"]) > 0

    def test_list_component_classes(self, tcp_connection):
        resp = tcp_connection.send_command("level.list_component_classes", {})
        data = resp["data"]
        assert "classes" in data
        assert data.get("count", len(data["classes"])) > 0

    def test_describe_class(self, tcp_connection):
        resp = tcp_connection.send_command(
            "level.describe_class", {"class": "PointLight"},
        )
        data = resp["data"]
        assert "class" in data
        assert "properties" in data
        assert "parent_class" in data


# ================================================================
# Actor Lifecycle (cleanup_actors fixture)
# ================================================================


@pytest.mark.e2e
class TestLevelActorLifecycle:

    def test_spawn_actor_basic(self, tcp_connection, cleanup_actors):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
        })
        data = resp["data"]
        assert "name" in data
        assert "label" in data
        assert "class" in data
        cleanup_actors.append(data["name"])

    def test_spawn_actor_with_label_and_folder(self, tcp_connection, cleanup_actors):
        label = _uniq("CortexE2E_labeled")
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": label,
            "folder": "CortexTest",
        })
        data = resp["data"]
        assert data["label"] == label
        assert "CortexTest" in data.get("folder", "")
        cleanup_actors.append(data["name"])

    def test_spawn_actor_with_transform(self, tcp_connection, cleanup_actors):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "location": [100.0, 200.0, 300.0],
            "rotation": [0.0, 45.0, 0.0],
            "scale": [2.0, 2.0, 2.0],
        })
        data = resp["data"]
        assert "name" in data
        cleanup_actors.append(data["name"])

    def test_duplicate_actor(self, tcp_connection, cleanup_actors):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_dup_src"),
        })
        src_name = resp["data"]["name"]
        cleanup_actors.append(src_name)

        dup_resp = tcp_connection.send_command("level.duplicate_actor", {
            "actor": src_name,
            "offset": [100.0, 0.0, 0.0],
        })
        dup_name = dup_resp["data"]["name"]
        assert dup_name != src_name
        cleanup_actors.append(dup_name)

    def test_rename_actor(self, tcp_connection, cleanup_actors):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_rename_old"),
        })
        name = resp["data"]["name"]
        cleanup_actors.append(name)

        new_label = _uniq("CortexE2E_rename_new")
        rename_resp = tcp_connection.send_command("level.rename_actor", {
            "actor": name,
            "label": new_label,
        })
        assert rename_resp["data"]["label"] == new_label

    def test_delete_actor(self, tcp_connection):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_delete"),
        })
        name = resp["data"]["name"]

        del_resp = tcp_connection.send_command("level.delete_actor", {
            "actor": name,
        })
        assert "name" in del_resp["data"]

    def test_delete_actor_with_confirm_class(self, tcp_connection):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_confirm_del"),
        })
        name = resp["data"]["name"]

        del_resp = tcp_connection.send_command("level.delete_actor", {
            "actor": name,
            "confirm_class": "PointLight",
        })
        assert "name" in del_resp["data"]


# ================================================================
# Transforms (actors_for_test fixture)
# ================================================================


@pytest.mark.e2e
class TestLevelTransforms:

    def test_get_actor(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.get_actor", {
            "actor": actors_for_test["light"],
        })
        data = resp["data"]
        assert "name" in data
        assert "label" in data
        assert "class" in data
        assert "location" in data
        assert "rotation" in data
        assert "scale" in data
        assert "components" in data

    def test_set_transform_location(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.set_transform", {
            "actor": actors_for_test["camera"],
            "location": [500.0, 600.0, 700.0],
        })
        data = resp["data"]
        assert "location" in data

    def test_set_transform_rotation_and_scale(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.set_transform", {
            "actor": actors_for_test["camera"],
            "rotation": [10.0, 20.0, 30.0],
            "scale": [1.5, 1.5, 1.5],
        })
        data = resp["data"]
        assert "rotation" in data
        assert "scale" in data

    def test_set_actor_property(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.set_actor_property", {
            "actor": actors_for_test["camera"],
            "property": "bHidden",
            "value": True,
        })
        data = resp["data"]
        assert "property" in data
        assert "value" in data

    def test_get_actor_property(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.get_actor_property", {
            "actor": actors_for_test["camera"],
            "property": "bHidden",
        })
        data = resp["data"]
        assert "property" in data
        assert "value" in data
        assert "type" in data


# ================================================================
# Components (actors_for_test fixture)
# ================================================================


@pytest.mark.e2e
class TestLevelComponents:

    def test_list_components(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.list_components", {
            "actor": actors_for_test["light"],
        })
        data = resp["data"]
        assert "components" in data
        assert data.get("count", len(data["components"])) > 0

    def test_add_component(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.add_component", {
            "actor": actors_for_test["mesh"],
            "class": "PointLightComponent",
        })
        data = resp["data"]
        assert "name" in data
        assert "class" in data

    def test_get_component_property(self, tcp_connection, actors_for_test):
        # Discover actual component name
        comps = tcp_connection.send_command("level.list_components", {
            "actor": actors_for_test["light"],
        })
        comp_list = comps["data"]["components"]
        light_comp = None
        for c in comp_list:
            cname = c.get("class", "") or c.get("type", "")
            if "PointLightComponent" in cname or "Light" in cname:
                light_comp = c.get("name", "")
                break
        assert light_comp, "No light component found"

        resp = tcp_connection.send_command("level.get_component_property", {
            "actor": actors_for_test["light"],
            "component": light_comp,
            "property": "Intensity",
        })
        data = resp["data"]
        assert "property" in data
        assert "value" in data

    def test_set_component_property(self, tcp_connection, actors_for_test):
        # Discover actual component name
        comps = tcp_connection.send_command("level.list_components", {
            "actor": actors_for_test["light"],
        })
        comp_list = comps["data"]["components"]
        light_comp = None
        for c in comp_list:
            cname = c.get("class", "") or c.get("type", "")
            if "PointLightComponent" in cname or "Light" in cname:
                light_comp = c.get("name", "")
                break
        assert light_comp, "No light component found"

        resp = tcp_connection.send_command("level.set_component_property", {
            "actor": actors_for_test["light"],
            "component": light_comp,
            "property": "Intensity",
            "value": 8000.0,
        })
        data = resp["data"]
        assert "property" in data
        assert "value" in data

    def test_remove_component(self, tcp_connection, actors_for_test):
        # Add an instance component then remove it
        add_resp = tcp_connection.send_command("level.add_component", {
            "actor": actors_for_test["mesh"],
            "class": "SpotLightComponent",
        })
        comp_name = add_resp["data"]["name"]

        resp = tcp_connection.send_command("level.remove_component", {
            "actor": actors_for_test["mesh"],
            "component": comp_name,
        })
        assert resp["success"] is True

    def test_remove_native_component_fails(self, tcp_connection, actors_for_test):
        # Discover a native component on the light actor
        comps = tcp_connection.send_command("level.list_components", {
            "actor": actors_for_test["light"],
        })
        comp_list = comps["data"]["components"]
        assert len(comp_list) > 0
        native_comp = comp_list[0].get("name", "")

        with pytest.raises(RuntimeError):
            tcp_connection.send_command("level.remove_component", {
                "actor": actors_for_test["light"],
                "component": native_comp,
            })


# ================================================================
# Queries (actors_for_test fixture)
# ================================================================


@pytest.mark.e2e
class TestLevelQueries:

    def test_list_actors(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.list_actors", {
            "limit": 50,
        })
        data = resp["data"]
        assert "actors" in data
        assert "count" in data
        assert "total" in data

    def test_list_actors_class_filter(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.list_actors", {
            "class": "PointLight",
        })
        data = resp["data"]
        assert "actors" in data
        assert len(data["actors"]) >= 1

    def test_find_actors(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.find_actors", {
            "pattern": "CortexE2E_*",
        })
        data = resp["data"]
        assert data.get("count", len(data.get("actors", []))) >= 3

    def test_find_actors_no_match(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.find_actors", {
            "pattern": "Nonexistent_12345_*",
        })
        data = resp["data"]
        assert data.get("count", len(data.get("actors", []))) == 0

    def test_get_bounds(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.get_bounds", {})
        data = resp["data"]
        assert "min" in data
        assert "max" in data
        assert "center" in data
        assert "extent" in data

    def test_select_actors(self, tcp_connection, actors_for_test):
        resp = tcp_connection.send_command("level.select_actors", {
            "actors": [actors_for_test["light"], actors_for_test["mesh"]],
        })
        data = resp["data"]
        assert data.get("count", 0) == 2

    def test_get_selection(self, tcp_connection, actors_for_test):
        # Self-contained: select first, then get
        tcp_connection.send_command("level.select_actors", {
            "actors": [actors_for_test["light"], actors_for_test["mesh"]],
        })
        resp = tcp_connection.send_command("level.get_selection", {})
        data = resp["data"]
        assert data.get("count", len(data.get("actors", []))) >= 2


# ================================================================
# Organization (cleanup_actors fixture)
# ================================================================


@pytest.mark.e2e
class TestLevelOrganization:

    def test_set_tags(self, tcp_connection, cleanup_actors):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_tags"),
        })
        name = resp["data"]["name"]
        cleanup_actors.append(name)

        tag_resp = tcp_connection.send_command("level.set_tags", {
            "actor": name,
            "tags": ["T1", "T2"],
        })
        assert len(tag_resp["data"].get("tags", [])) == 2

    def test_set_folder(self, tcp_connection, cleanup_actors):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_folder"),
        })
        name = resp["data"]["name"]
        cleanup_actors.append(name)

        folder_resp = tcp_connection.send_command("level.set_folder", {
            "actor": name,
            "folder": "CortexTest/Sub",
        })
        assert "CortexTest/Sub" in folder_resp["data"].get("folder", "")

    def test_set_folder_empty_clears(self, tcp_connection, cleanup_actors):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_folder_clear"),
        })
        name = resp["data"]["name"]
        cleanup_actors.append(name)

        tcp_connection.send_command("level.set_folder", {
            "actor": name, "folder": "CortexTest/Temp",
        })
        clear_resp = tcp_connection.send_command("level.set_folder", {
            "actor": name, "folder": "",
        })
        assert clear_resp["data"].get("folder", "") == ""

    def test_attach_detach_actor(self, tcp_connection, cleanup_actors):
        child = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_child"),
        })
        parent = tcp_connection.send_command("level.spawn_actor", {
            "class": "StaticMeshActor",
            "label": _uniq("CortexE2E_parent"),
        })
        child_name = child["data"]["name"]
        parent_name = parent["data"]["name"]
        cleanup_actors.append(child_name)
        cleanup_actors.append(parent_name)

        tcp_connection.send_command("level.attach_actor", {
            "actor": child_name, "parent": parent_name,
        })
        tcp_connection.send_command("level.detach_actor", {
            "actor": child_name,
        })

    def test_attach_verify_via_get_actor(self, tcp_connection, cleanup_actors):
        child = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_att_child"),
        })
        parent = tcp_connection.send_command("level.spawn_actor", {
            "class": "StaticMeshActor",
            "label": _uniq("CortexE2E_att_parent"),
        })
        child_name = child["data"]["name"]
        parent_name = parent["data"]["name"]
        cleanup_actors.append(child_name)
        cleanup_actors.append(parent_name)

        tcp_connection.send_command("level.attach_actor", {
            "actor": child_name, "parent": parent_name,
        })
        info = tcp_connection.send_command("level.get_actor", {
            "actor": child_name,
        })
        assert info["data"].get("parent", "") != ""

        # Cleanup: detach
        tcp_connection.send_command("level.detach_actor", {"actor": child_name})

    def test_group_actors(self, tcp_connection, cleanup_actors):
        # Check for World Partition
        info = tcp_connection.send_command("level.get_info", {})
        if info["data"].get("is_world_partition", False):
            pytest.skip("Grouping not supported on World Partition levels")

        a1 = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_grp1"),
        })
        a2 = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_grp2"),
        })
        n1 = a1["data"]["name"]
        n2 = a2["data"]["name"]
        cleanup_actors.append(n1)
        cleanup_actors.append(n2)

        resp = tcp_connection.send_command("level.group_actors", {
            "actors": [n1, n2],
        })
        data = resp["data"]
        assert "group" in data
        assert "count" in data

    def test_ungroup_actors(self, tcp_connection, cleanup_actors):
        info = tcp_connection.send_command("level.get_info", {})
        if info["data"].get("is_world_partition", False):
            pytest.skip("Grouping not supported on World Partition levels")

        a1 = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_ugrp1"),
        })
        a2 = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_ugrp2"),
        })
        n1 = a1["data"]["name"]
        n2 = a2["data"]["name"]
        cleanup_actors.append(n1)
        cleanup_actors.append(n2)

        grp = tcp_connection.send_command("level.group_actors", {
            "actors": [n1, n2],
        })
        group_name = grp["data"]["group"]

        resp = tcp_connection.send_command("level.ungroup_actors", {
            "group": group_name,
        })
        assert "count" in resp["data"]


# ================================================================
# Streaming / Read-Only
#
# Note: load_sublevel, unload_sublevel, set_sublevel_visibility, and
# set_data_layer are intentionally excluded — they require project-specific
# sublevel/data-layer content and are environment-dependent. Covered
# conditionally in MCP Scenario 8 and the benchmark skill when available.
# ================================================================


@pytest.mark.e2e
class TestLevelStreamingReadOnly:

    def test_get_info(self, tcp_connection):
        resp = tcp_connection.send_command("level.get_info", {})
        data = resp["data"]
        assert "level_name" in data
        assert "actor_count" in data

    def test_list_sublevels(self, tcp_connection):
        resp = tcp_connection.send_command("level.list_sublevels", {})
        data = resp["data"]
        assert "sublevels" in data
        assert "count" in data

    def test_list_data_layers(self, tcp_connection):
        try:
            resp = tcp_connection.send_command("level.list_data_layers", {})
        except RuntimeError as exc:
            if "code: EDITOR_NOT_READY" in str(exc):
                pytest.skip("DataLayerEditorSubsystem unavailable in current editor context")
            raise
        data = resp["data"]
        assert "data_layers" in data
        assert "count" in data


# ================================================================
# Save (runs last, restores map after)
# ================================================================


@pytest.mark.e2e
class TestLevelSave:

    @pytest.fixture(autouse=True, scope="class")
    def _restore_map(self):
        """Restore map file after save tests to avoid polluting content."""
        yield
        project_root = Path(__file__).resolve().parents[4]
        try:
            subprocess.run(
                ["git", "checkout", "Content/"],
                cwd=str(project_root),
                capture_output=True,
                timeout=30,
            )
        except Exception:
            pass

    def test_save_level(self, tcp_connection):
        resp = tcp_connection.send_command("level.save_level", {})
        data = resp["data"]
        assert data.get("saved") is True or resp["success"] is True

    def test_save_all(self, tcp_connection):
        pytest.skip("save_all is unstable in unattended e2e (can block on asset save workflows)")


# ================================================================
# Error Cases
# ================================================================


@pytest.mark.e2e
class TestLevelErrors:

    def test_spawn_invalid_class(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("level.spawn_actor", {
                "class": "NonExistentClass_12345",
            })

    def test_delete_nonexistent_actor(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("level.delete_actor", {
                "actor": "NonExistentActor_12345",
            })

    def test_delete_wrong_confirm_class(self, tcp_connection):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_wrong_confirm"),
        })
        name = resp["data"]["name"]
        try:
            with pytest.raises(RuntimeError):
                tcp_connection.send_command("level.delete_actor", {
                    "actor": name,
                    "confirm_class": "CameraActor",
                })
        finally:
            try:
                tcp_connection.send_command("level.delete_actor", {"actor": name})
            except (RuntimeError, ConnectionError):
                pass

    def test_get_actor_nonexistent(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("level.get_actor", {
                "actor": "NonExistentActor_12345",
            })

    def test_describe_class_nonexistent(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("level.describe_class", {
                "class": "NonExistentClass_12345",
            })

    def test_set_actor_property_invalid(self, tcp_connection):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_bad_prop"),
        })
        name = resp["data"]["name"]
        try:
            with pytest.raises(RuntimeError):
                tcp_connection.send_command("level.set_actor_property", {
                    "actor": name,
                    "property": "NonExistentProp_12345",
                    "value": True,
                })
        finally:
            try:
                tcp_connection.send_command("level.delete_actor", {"actor": name})
            except (RuntimeError, ConnectionError):
                pass

    def test_add_component_invalid_class(self, tcp_connection):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_bad_comp"),
        })
        name = resp["data"]["name"]
        try:
            with pytest.raises(RuntimeError):
                tcp_connection.send_command("level.add_component", {
                    "actor": name,
                    "class": "NonExistentComponent_12345",
                })
        finally:
            try:
                tcp_connection.send_command("level.delete_actor", {"actor": name})
            except (RuntimeError, ConnectionError):
                pass

    def test_attach_nonexistent_parent(self, tcp_connection):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_orphan"),
        })
        name = resp["data"]["name"]
        try:
            with pytest.raises(RuntimeError):
                tcp_connection.send_command("level.attach_actor", {
                    "actor": name,
                    "parent": "NonExistentActor_12345",
                })
        finally:
            try:
                tcp_connection.send_command("level.delete_actor", {"actor": name})
            except (RuntimeError, ConnectionError):
                pass

    def test_get_component_property_invalid(self, tcp_connection):
        resp = tcp_connection.send_command("level.spawn_actor", {
            "class": "PointLight",
            "label": _uniq("CortexE2E_bad_comp_prop"),
        })
        name = resp["data"]["name"]
        try:
            with pytest.raises(RuntimeError):
                tcp_connection.send_command("level.get_component_property", {
                    "actor": name,
                    "component": "NonExistentComponent_12345",
                    "property": "Intensity",
                })
        finally:
            try:
                tcp_connection.send_command("level.delete_actor", {"actor": name})
            except (RuntimeError, ConnectionError):
                pass
