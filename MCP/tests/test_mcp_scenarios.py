"""MCP Scenario Tests — Cross-domain workflows via FastMCP test client.

Tests the full Python MCP stack: tool registration, parameter validation,
response formatting, and caching. Each scenario exercises a multi-step
workflow across domains, asserting intermediate state at each step.

Requires a running Unreal Editor with UnrealCortex plugin.

Run:
    cd Plugins/UnrealCortex/MCP && uv run pytest tests/test_mcp_scenarios.py -v
    cd Plugins/UnrealCortex/MCP && uv run pytest tests/test_mcp_scenarios.py -v -k "not stress"
    cd Plugins/UnrealCortex/MCP && uv run pytest tests/test_mcp_scenarios.py -v -k stress
"""

import json
import os
from pathlib import Path
import uuid

import pytest


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


async def call_tool(client, name: str, args: dict) -> dict:
    """Call an MCP tool and return parsed JSON response."""
    result = await client.call_tool(name, args)
    return json.loads(result.content[0].text)


async def cleanup_blueprint(client, asset_path: str) -> None:
    """Best-effort delete of a test blueprint."""
    try:
        await client.call_tool("delete_blueprint", {"asset_path": asset_path})
    except Exception:
        pass


def _uniq(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex[:8]}"


# ================================================================
# Scenario 1: Blueprint Lifecycle (Blueprint + Graph)
# ================================================================


@pytest.mark.anyio
@pytest.mark.scenario
async def test_scenario_blueprint_lifecycle(mcp_client):
    """Create Actor BP -> add variable/function -> wire graph -> compile -> verify -> delete."""
    asset_path = None
    try:
        # Step 1: Create blueprint
        data = await call_tool(mcp_client, "create_blueprint", {
            "name": _uniq("MCPTest_Actor"),
            "path": "/Game/Temp/CortexMCPTest",
            "type": "Actor",
        })
        assert "asset_path" in data
        asset_path = data["asset_path"]

        # Step 2: Add variable
        await call_tool(mcp_client, "add_blueprint_variable", {
            "asset_path": asset_path,
            "name": "Health",
            "type": "Float",
        })

        # Step 3: Add function
        await call_tool(mcp_client, "add_blueprint_function", {
            "asset_path": asset_path,
            "name": "TakeDamage",
        })

        # Step 4: Verify graphs (EventGraph + TakeDamage function graph)
        data = await call_tool(mcp_client, "graph_list_graphs", {
            "asset_path": asset_path,
        })
        assert "graphs" in data
        graph_names = [g["name"] for g in data["graphs"]]
        assert "EventGraph" in graph_names

        # Step 5: Add PrintString node to EventGraph
        data = await call_tool(mcp_client, "graph_add_node", {
            "asset_path": asset_path,
            "node_class": "UK2Node_CallFunction",
            "graph_name": "EventGraph",
            "params": json.dumps({"function_name": "KismetSystemLibrary.PrintString"}),
        })
        assert "node_id" in data
        print_node_id = data["node_id"]

        # Step 6: List nodes to find BeginPlay event
        data = await call_tool(mcp_client, "graph_list_nodes", {
            "asset_path": asset_path,
            "graph_name": "EventGraph",
        })
        event_node = None
        for n in data["nodes"]:
            if "BeginPlay" in n.get("title", "") or "BeginPlay" in n.get("class", ""):
                event_node = n
                break

        # Step 7: Connect BeginPlay -> PrintString (if found)
        if event_node:
            await call_tool(mcp_client, "graph_connect", {
                "asset_path": asset_path,
                "source_node": event_node["node_id"],
                "source_pin": "then",
                "target_node": print_node_id,
                "target_pin": "execute",
                "graph_name": "EventGraph",
            })

        # Step 8: Compile
        await call_tool(mcp_client, "compile_blueprint", {
            "asset_path": asset_path,
        })

        # Step 9: Verify all additions via get_blueprint_info
        data = await call_tool(mcp_client, "get_blueprint_info", {
            "asset_path": asset_path,
        })
        var_names = [v["name"] for v in data.get("variables", [])]
        func_names = [f["name"] for f in data.get("functions", [])]
        assert "Health" in var_names
        assert "TakeDamage" in func_names

    finally:
        if asset_path:
            await cleanup_blueprint(mcp_client, asset_path)


# ================================================================
# Scenario 2: Widget Builder (Blueprint + UMG)
# ================================================================


@pytest.mark.anyio
@pytest.mark.scenario
async def test_scenario_widget_builder(mcp_client):
    """Create Widget BP -> build widget tree -> style -> verify -> cleanup."""
    asset_path = None
    try:
        # Step 1: Create Widget Blueprint
        data = await call_tool(mcp_client, "create_blueprint", {
            "name": _uniq("MCPTest_Widget"),
            "path": "/Game/Temp/CortexMCPTest",
            "type": "Widget",
        })
        assert "asset_path" in data
        asset_path = data["asset_path"]

        # Step 2: Add root CanvasPanel
        await call_tool(mcp_client, "add_widget", {
            "asset_path": asset_path,
            "widget_class": "CanvasPanel",
            "name": "RootPanel",
        })

        # Step 3: Add TextBlock child
        await call_tool(mcp_client, "add_widget", {
            "asset_path": asset_path,
            "widget_class": "TextBlock",
            "name": "TitleText",
            "parent_name": "RootPanel",
        })

        # Step 4: Add Button child
        await call_tool(mcp_client, "add_widget", {
            "asset_path": asset_path,
            "widget_class": "Button",
            "name": "ActionButton",
            "parent_name": "RootPanel",
        })

        # Step 5: Set text
        await call_tool(mcp_client, "set_text", {
            "asset_path": asset_path,
            "widget_name": "TitleText",
            "text": "Hello World",
        })

        # Step 6: Set color
        await call_tool(mcp_client, "set_color", {
            "asset_path": asset_path,
            "widget_name": "TitleText",
            "color": "#FF0000",
        })

        # Step 7: Set anchor
        await call_tool(mcp_client, "set_anchor", {
            "asset_path": asset_path,
            "widget_name": "ActionButton",
            "preset": "BottomCenter",
        })

        # Step 8: Verify tree
        data = await call_tool(mcp_client, "get_tree", {
            "asset_path": asset_path,
        })
        assert data  # Should have tree content

        # Step 9: Duplicate widget
        await call_tool(mcp_client, "duplicate_widget", {
            "asset_path": asset_path,
            "widget_name": "ActionButton",
            "new_name": "ActionButton_Copy",
        })

        # Step 10: Save
        await call_tool(mcp_client, "save_blueprint", {
            "asset_path": asset_path,
        })

    finally:
        if asset_path:
            await cleanup_blueprint(mcp_client, asset_path)


# ================================================================
# Scenario 3: Data Pipeline
# ================================================================


@pytest.mark.anyio
@pytest.mark.scenario
async def test_scenario_data_pipeline(mcp_client):
    """List tables -> schema -> query -> add row -> search -> batch -> delete."""
    test_row_name = "MCPScenario_TestRow"
    table_path = "/Game/Data/DT_TestSimple"

    # Step 1: List datatables
    data = await call_tool(mcp_client, "list_datatables", {})
    assert "datatables" in data
    assert len(data["datatables"]) > 0

    # Step 2: Get schema
    data = await call_tool(mcp_client, "get_datatable_schema", {
        "table_path": table_path,
    })
    assert "schema" in data
    assert "fields" in data["schema"]

    # Step 3: Query all rows
    data = await call_tool(mcp_client, "query_datatable", {
        "table_path": table_path,
    })
    assert "rows" in data

    try:
        # Step 4: Add test row
        data = await call_tool(mcp_client, "add_datatable_row", {
            "table_path": table_path,
            "row_name": test_row_name,
            "row_data": json.dumps({"Tag": "Cortex.Test.Tag1"}),
        })
        assert data["row_name"] == test_row_name

        # Step 5: Get the added row
        data = await call_tool(mcp_client, "get_datatable_row", {
            "table_path": table_path,
            "row_name": test_row_name,
        })
        assert data["row_name"] == test_row_name

        # Step 6: Update the row
        await call_tool(mcp_client, "update_datatable_row", {
            "table_path": table_path,
            "row_name": test_row_name,
            "row_data": json.dumps({"Tag": "Cortex.Test.Tag2"}),
        })

        # Step 7: Search for the row
        data = await call_tool(mcp_client, "search_datatable_content", {
            "table_path": table_path,
            "search_text": "Cortex.Test.Tag2",
        })
        assert data["total_matches"] >= 1
        assert any(r["row_name"] == test_row_name for r in data["results"])

        # Step 8: Batch query
        data = await call_tool(mcp_client, "batch_query", {
            "commands": json.dumps([
                {"command": "data.get_datatable_schema", "params": {"table_path": table_path}},
                {"command": "data.query_datatable", "params": {"table_path": table_path}},
                {"command": "data.search_datatable_content", "params": {"table_path": table_path, "search_text": test_row_name}},
            ]),
        })
        assert "results" in data
        assert len(data["results"]) == 3

    finally:
        # Step 9: Cleanup
        try:
            await call_tool(mcp_client, "delete_datatable_row", {
                "table_path": table_path,
                "row_name": test_row_name,
            })
        except Exception:
            pass

# ================================================================
# Scenario 4: Graph Wiring
# ================================================================


@pytest.mark.anyio
@pytest.mark.scenario
async def test_scenario_graph_wiring(mcp_client):
    """Create BP -> add nodes -> wire connections -> verify -> disconnect -> cleanup."""
    asset_path = None
    try:
        data = await call_tool(mcp_client, "create_blueprint", {
            "name": _uniq("MCPTest_GraphBP"),
            "path": "/Game/Temp/CortexMCPTest",
            "type": "Actor",
        })
        asset_path = data["asset_path"]

        # List graphs
        data = await call_tool(mcp_client, "graph_list_graphs", {
            "asset_path": asset_path,
        })
        assert "EventGraph" in [g["name"] for g in data["graphs"]]

        # Add PrintString node
        data = await call_tool(mcp_client, "graph_add_node", {
            "asset_path": asset_path,
            "node_class": "UK2Node_CallFunction",
            "graph_name": "EventGraph",
            "params": json.dumps({"function_name": "KismetSystemLibrary.PrintString"}),
        })
        print_id = data["node_id"]

        # Add second PrintString node
        data = await call_tool(mcp_client, "graph_add_node", {
            "asset_path": asset_path,
            "node_class": "UK2Node_CallFunction",
            "graph_name": "EventGraph",
            "params": json.dumps({"function_name": "KismetSystemLibrary.PrintString"}),
        })
        second_print_id = data["node_id"]

        # List nodes
        data = await call_tool(mcp_client, "graph_list_nodes", {
            "asset_path": asset_path,
            "graph_name": "EventGraph",
        })
        assert len(data["nodes"]) >= 2

        # Get second node details
        data = await call_tool(mcp_client, "graph_get_node", {
            "asset_path": asset_path,
            "node_id": second_print_id,
            "graph_name": "EventGraph",
        })
        assert "pins" in data

        # Try connect
        try:
            await call_tool(mcp_client, "graph_connect", {
                "asset_path": asset_path,
                "source_node": print_id,
                "source_pin": "then",
                "target_node": second_print_id,
                "target_pin": "execute",
                "graph_name": "EventGraph",
            })
        except Exception:
            pass

        # Disconnect
        await call_tool(mcp_client, "graph_disconnect", {
            "asset_path": asset_path,
            "node_id": print_id,
            "pin_name": "then",
            "graph_name": "EventGraph",
        })

    finally:
        if asset_path:
            await cleanup_blueprint(mcp_client, asset_path)


# ================================================================
# Scenario 5: GameplayTag Workflow
# ================================================================


@pytest.mark.anyio
@pytest.mark.scenario
async def test_scenario_gameplay_tags(mcp_client):
    """List tags -> register -> validate -> bulk register -> verify."""
    project_root = Path(__file__).resolve().parents[4]
    test_ini_rel = "Tags/GameplayTags_MCPScenario.ini"
    test_ini_path = project_root / "Config" / test_ini_rel
    original_ini = test_ini_path.read_text(encoding="utf-8") if test_ini_path.exists() else None

    # Baseline
    data = await call_tool(mcp_client, "list_gameplay_tags", {})
    assert "tags" in data

    try:
        # Register a test tag in dedicated test ini file
        benchmark_tag = _uniq("Cortex.Test.MCPBenchmark")
        data = await call_tool(mcp_client, "register_gameplay_tag", {
            "tag": benchmark_tag,
            "ini_file": test_ini_rel,
        })
        assert data.get("success") is True
        assert data.get("ini_file", "").replace("\\", "/").endswith("Config/Tags/GameplayTags_MCPScenario.ini")

        # Validate registered tag response shape (may require restart to become valid)
        data = await call_tool(mcp_client, "validate_gameplay_tag", {
            "tag": benchmark_tag,
        })
        assert "valid" in data

        # Validate non-existent tag
        data = await call_tool(mcp_client, "validate_gameplay_tag", {
            "tag": "Cortex.Test.NonExistent_12345",
        })
        assert data["valid"] is False

        # Verify listing response shape
        data = await call_tool(mcp_client, "list_gameplay_tags", {"prefix": "Cortex.Test"})
        assert "tags" in data
    finally:
        if original_ini is None:
            if test_ini_path.exists():
                test_ini_path.unlink()
        else:
            test_ini_path.parent.mkdir(parents=True, exist_ok=True)
            test_ini_path.write_text(original_ini, encoding="utf-8")


# ================================================================
# Scenario 6: Localization Pipeline
# ================================================================


@pytest.mark.anyio
@pytest.mark.scenario
async def test_scenario_localization(mcp_client):
    """List string tables -> get translations -> set -> verify -> overwrite -> verify."""
    table_path = "/Game/Data/ST_TestStrings"

    # List string tables
    data = await call_tool(mcp_client, "list_string_tables", {})
    assert "string_tables" in data
    assert len(data["string_tables"]) > 0

    # Get baseline translations
    data = await call_tool(mcp_client, "get_translations", {
        "string_table_path": table_path,
    })
    assert "entries" in data

    # Set a new translation
    key = _uniq("MCPScenario_TestKey")
    await call_tool(mcp_client, "set_translation", {
        "string_table_path": table_path,
        "key": key,
        "text": "Hello from MCP",
    })

    # Verify
    data = await call_tool(mcp_client, "get_translations", {
        "string_table_path": table_path,
    })
    keys = [e["key"] for e in data["entries"]]
    assert key in keys

    # Overwrite
    await call_tool(mcp_client, "set_translation", {
        "string_table_path": table_path,
        "key": key,
        "text": "Updated Hello from MCP",
    })

    try:
        # Verify update
        data = await call_tool(mcp_client, "get_translations", {
            "string_table_path": table_path,
        })
        for entry in data["entries"]:
            if entry["key"] == key:
                value = entry.get("source_string") or entry.get("text", "")
                assert value == "Updated Hello from MCP"
                break
        else:
            pytest.fail(f"{key} not found after update")
    finally:
        # Cleanup: remove test translation entry
        try:
            await call_tool(mcp_client, "remove_translation", {
                "string_table_path": table_path,
                "key": key,
            })
        except Exception:
            pass  # remove_translation may not exist; unique key prevents pollution

# ================================================================
# Scenario 7: Level Scene Construction
# ================================================================


async def _cleanup_actor(client, actor: str) -> None:
    """Best-effort delete of a test actor."""
    try:
        await client.call_tool("delete_actor", {"actor": actor})
    except Exception:
        pass


async def _sweep_actors(client, pattern: str) -> None:
    """Find and delete all actors matching pattern (safety net)."""
    try:
        data = await call_tool(client, "find_actors", {"pattern": pattern})
        for actor in data.get("actors", []):
            name = actor.get("name") or actor.get("label", "")
            if name:
                await _cleanup_actor(client, name)
    except Exception:
        pass


@pytest.mark.anyio
@pytest.mark.scenario
async def test_scenario_level_scene(mcp_client):
    """Spawn actors, transform, tag, organize, query, components, describe, cleanup."""
    light_name = None
    mesh_name = None
    added_comp_name = None
    try:
        # Step 1: get_info — baseline
        data = await call_tool(mcp_client, "get_info", {})
        assert "level_name" in data

        # Step 2: list_actor_classes — discovery
        data = await call_tool(mcp_client, "list_actor_classes", {})
        assert "classes" in data

        # Step 3: spawn PointLight
        light_label = _uniq("MCPScenario_light")
        data = await call_tool(mcp_client, "spawn_actor", {
            "class_name": "PointLight",
            "label": light_label,
        })
        assert "name" in data
        light_name = data["name"]

        # Step 4: spawn StaticMeshActor
        mesh_label = _uniq("MCPScenario_mesh")
        data = await call_tool(mcp_client, "spawn_actor", {
            "class_name": "StaticMeshActor",
            "label": mesh_label,
        })
        assert "name" in data
        mesh_name = data["name"]

        # Step 5: set_transform on LIGHT
        await call_tool(mcp_client, "set_transform", {
            "actor": light_name,
            "location": [100.0, 200.0, 300.0],
        })

        # Step 6: set_tags on LIGHT
        data = await call_tool(mcp_client, "set_tags", {
            "actor": light_name,
            "tags": ["ScenarioTag"],
        })
        assert len(data.get("tags", [])) == 1

        # Step 7: set_folder on LIGHT
        await call_tool(mcp_client, "set_folder", {
            "actor": light_name,
            "folder": "CortexScenario",
        })

        # Step 8: attach LIGHT to MESH
        await call_tool(mcp_client, "attach_actor", {
            "actor": light_name,
            "parent": mesh_name,
        })

        # Step 9: get_actor on LIGHT — verify attachment
        data = await call_tool(mcp_client, "get_actor", {
            "actor": light_name,
        })
        assert data.get("parent", "") != ""

        # Step 10: find_actors by pattern
        data = await call_tool(mcp_client, "find_actors", {
            "pattern": "MCPScenario_*",
        })
        assert data.get("count", len(data.get("actors", []))) >= 2

        # Step 11: select_actors
        await call_tool(mcp_client, "select_actors", {
            "actors": [light_name, mesh_name],
        })

        # Step 12: get_selection — verify
        data = await call_tool(mcp_client, "get_selection", {})
        assert data.get("count", len(data.get("actors", []))) >= 2

        # Step 13: list_components on MESH
        data = await call_tool(mcp_client, "list_components", {
            "actor": mesh_name,
        })
        assert "components" in data

        # Step 14: add_component on MESH
        data = await call_tool(mcp_client, "add_component", {
            "actor": mesh_name,
            "class_name": "PointLightComponent",
        })
        assert "name" in data
        added_comp_name = data["name"]

        # Step 15: remove_component — cleanup added component
        await call_tool(mcp_client, "remove_component", {
            "actor": mesh_name,
            "component": added_comp_name,
        })
        added_comp_name = None

        # Step 16: describe_class PointLight
        data = await call_tool(mcp_client, "describe_class", {
            "class_name": "PointLight",
        })
        assert "class" in data
        assert "properties" in data

        # Step 17: detach LIGHT
        await call_tool(mcp_client, "detach_actor", {
            "actor": light_name,
        })

    finally:
        if light_name:
            await _cleanup_actor(mcp_client, light_name)
        if mesh_name:
            await _cleanup_actor(mcp_client, mesh_name)
        await _sweep_actors(mcp_client, "MCPScenario_*")


# ================================================================
# Scenario 8: Composite Level Scene
# ================================================================


@pytest.mark.anyio
@pytest.mark.scenario
async def test_scenario_composite_level_scene(mcp_client):
    """Create a multi-actor scene via create_level_scene composite tool."""
    spawned_actors = []
    try:
        # Step 1: create_level_scene with 3 actors + attachment
        data = await call_tool(mcp_client, "create_level_scene", {
            "actors": [
                {
                    "id": "light",
                    "class": "PointLight",
                    "label": _uniq("MCPScenario_comp_light"),
                    "location": [0.0, 0.0, 500.0],
                },
                {
                    "id": "mesh",
                    "class": "StaticMeshActor",
                    "label": _uniq("MCPScenario_comp_mesh"),
                    "location": [200.0, 0.0, 0.0],
                },
                {
                    "id": "camera",
                    "class": "CameraActor",
                    "label": _uniq("MCPScenario_comp_cam"),
                    "location": [-500.0, 0.0, 200.0],
                },
            ],
            "organization": {
                "attachments": [
                    {"child": "light", "parent": "mesh"},
                ],
            },
            "save": False,
        })

        # Step 2: Verify
        assert data.get("actor_count", 0) == 3
        spawned_actors = data.get("spawned_actors", [])
        assert len(spawned_actors) == 3
        # 3 spawns + 1 attachment = at least 4 completed steps
        assert data.get("completed_steps", 0) >= 4

    finally:
        # Step 3: Delete all spawned actors
        for actor in reversed(spawned_actors):
            await _cleanup_actor(mcp_client, actor)
        # Step 4: Safety net sweep
        await _sweep_actors(mcp_client, "MCPScenario_comp_*")


# ================================================================
# Stress Tests
# ================================================================


@pytest.mark.anyio
@pytest.mark.stress
async def test_stress_bulk_blueprint_create(mcp_client):
    """Create 20 BPs, list them, then delete all."""
    created = []
    try:
        for i in range(20):
            data = await call_tool(mcp_client, "create_blueprint", {
                "name": _uniq(f"MCPStress_BP_{i:03d}"),
                "path": "/Game/Temp/CortexMCPTest/Stress",
                "type": "Actor",
            })
            assert "asset_path" in data
            created.append(data["asset_path"])

        data = await call_tool(mcp_client, "list_blueprints", {
            "path": "/Game/Temp/CortexMCPTest/Stress",
        })
        assert len(data["blueprints"]) >= 20

    finally:
        for path in created:
            await cleanup_blueprint(mcp_client, path)


@pytest.mark.anyio
@pytest.mark.stress
async def test_stress_large_widget_tree(mcp_client):
    """Build a 50-widget hierarchy, get_tree, verify structure."""
    asset_path = None
    try:
        data = await call_tool(mcp_client, "create_blueprint", {
            "name": _uniq("MCPStress_WidgetTree"),
            "path": "/Game/Temp/CortexMCPTest/Stress",
            "type": "Widget",
        })
        asset_path = data["asset_path"]

        await call_tool(mcp_client, "add_widget", {
            "asset_path": asset_path,
            "widget_class": "CanvasPanel",
            "name": "StressRoot",
        })

        for i in range(49):
            await call_tool(mcp_client, "add_widget", {
                "asset_path": asset_path,
                "widget_class": "TextBlock",
                "name": f"StressWidget_{i:03d}",
                "parent_name": "StressRoot",
            })

        data = await call_tool(mcp_client, "get_tree", {
            "asset_path": asset_path,
        })
        assert data

    finally:
        if asset_path:
            await cleanup_blueprint(mcp_client, asset_path)


@pytest.mark.anyio
@pytest.mark.stress
async def test_stress_many_graph_nodes(mcp_client):
    """Add 30 nodes, connect in chain, verify."""
    asset_path = None
    try:
        data = await call_tool(mcp_client, "create_blueprint", {
            "name": _uniq("MCPStress_GraphNodes"),
            "path": "/Game/Temp/CortexMCPTest/Stress",
            "type": "Actor",
        })
        asset_path = data["asset_path"]

        node_ids = []
        for i in range(30):
            data = await call_tool(mcp_client, "graph_add_node", {
                "asset_path": asset_path,
                "node_class": "UK2Node_CallFunction",
                "graph_name": "EventGraph",
                "params": json.dumps({"function_name": "KismetSystemLibrary.PrintString"}),
            })
            node_ids.append(data["node_id"])

        for i in range(len(node_ids) - 1):
            try:
                await call_tool(mcp_client, "graph_connect", {
                    "asset_path": asset_path,
                    "source_node": node_ids[i],
                    "source_pin": "then",
                    "target_node": node_ids[i + 1],
                    "target_pin": "execute",
                    "graph_name": "EventGraph",
                })
            except Exception:
                break

        data = await call_tool(mcp_client, "graph_list_nodes", {
            "asset_path": asset_path,
            "graph_name": "EventGraph",
        })
        assert len(data["nodes"]) >= 30

    finally:
        if asset_path:
            await cleanup_blueprint(mcp_client, asset_path)


@pytest.mark.anyio
@pytest.mark.stress
async def test_stress_rapid_data_operations(mcp_client):
    """100 add/update/delete cycles on a DataTable."""
    table_path = "/Game/Data/DT_TestSimple"

    failures = 0
    for i in range(100):
        row_name = f"MCPStress_Row_{i:04d}"
        try:
            await call_tool(mcp_client, "add_datatable_row", {
                "table_path": table_path,
                "row_name": row_name,
                "row_data": json.dumps({"Tag": "Cortex.Test.Tag1"}),
            })
            await call_tool(mcp_client, "update_datatable_row", {
                "table_path": table_path,
                "row_name": row_name,
                "row_data": json.dumps({"Tag": "Cortex.Test.Tag2"}),
            })
            await call_tool(mcp_client, "delete_datatable_row", {
                "table_path": table_path,
                "row_name": row_name,
            })
        except Exception:
            failures += 1
            try:
                await call_tool(mcp_client, "delete_datatable_row", {
                    "table_path": table_path,
                    "row_name": row_name,
                })
            except Exception:
                pass
    assert failures < 5, f"Too many failures: {failures}/100"


@pytest.mark.anyio
@pytest.mark.stress
async def test_stress_concurrent_batch(mcp_client):
    """Batch query with 20 commands."""
    commands = [{"command": "get_status", "params": {}} for _ in range(20)]

    data = await call_tool(mcp_client, "batch_query", {
        "commands": json.dumps(commands),
    })
    assert "results" in data
    assert len(data["results"]) == 20
    assert all(r.get("success") for r in data["results"])


@pytest.mark.anyio
@pytest.mark.stress
async def test_stress_rapid_actor_lifecycle(mcp_client):
    """50 spawn+delete cycles to test actor lifecycle throughput."""
    spawned_not_deleted = []
    failures = 0
    try:
        for i in range(50):
            label = _uniq(f"MCPStress_actor_{i:03d}")
            try:
                data = await call_tool(mcp_client, "spawn_actor", {
                    "class_name": "PointLight",
                    "label": label,
                })
                name = data.get("name", "")
                spawned_not_deleted.append(name)
                await call_tool(mcp_client, "delete_actor", {"actor": name})
                spawned_not_deleted.remove(name)
            except Exception:
                failures += 1

        assert failures < 3, f"Too many failures: {failures}/50"

    finally:
        # Clean up any actors that were spawned but not deleted
        for actor in spawned_not_deleted:
            await _cleanup_actor(mcp_client, actor)
        await _sweep_actors(mcp_client, "MCPStress_actor_*")


@pytest.mark.anyio
@pytest.mark.stress
async def test_stress_bulk_scene_composite(mcp_client):
    """Create a 20-actor scene via composite tool."""
    spawned_actors = []
    try:
        actors = [
            {
                "id": f"light_{i:03d}",
                "class": "PointLight",
                "label": _uniq(f"MCPStress_scene_{i:03d}"),
                "location": [float(i * 200), 0.0, 500.0],
            }
            for i in range(20)
        ]

        data = await call_tool(mcp_client, "create_level_scene", {
            "actors": actors,
            "save": False,
        })
        spawned_actors = data.get("spawned_actors", [])
        assert data.get("actor_count", 0) >= 20

        # Verify via list_actors
        data = await call_tool(mcp_client, "list_actors", {
            "class_filter": "PointLight",
        })
        assert data.get("count", 0) >= 20

    finally:
        for actor in reversed(spawned_actors):
            await _cleanup_actor(mcp_client, actor)
        await _sweep_actors(mcp_client, "MCPStress_scene_*")


# ================================================================
# Scenario 9: Editor Viewport Workflow (non-PIE)
# ================================================================


@pytest.mark.anyio
@pytest.mark.scenario
async def test_scenario_editor_viewport(mcp_client):
    """Verify editor state -> move camera -> screenshot -> change view mode -> restore."""
    original_mode = None
    screenshot_path = None

    # Step 1: Verify connected. Self-heal if PIE is running.
    data = await call_tool(mcp_client, "get_editor_state", {})
    assert "project_name" in data
    if data.get("pie_state") != "stopped":
        await call_tool(mcp_client, "stop_pie", {})
        data = await call_tool(mcp_client, "get_editor_state", {})
        assert data["pie_state"] == "stopped"

    # Step 2: Capture baseline view mode for restore in step 5.
    data = await call_tool(mcp_client, "get_viewport_info", {})
    assert "view_mode" in data
    original_mode = data["view_mode"].lower()

    # Step 3: Set camera position and read back.
    await call_tool(mcp_client, "set_viewport_camera", {
        "x": 800.0, "y": 400.0, "z": 300.0,
    })
    data = await call_tool(mcp_client, "get_viewport_info", {})
    cam = data["camera_location"]
    assert cam["x"] == pytest.approx(800.0, abs=1.0)
    assert cam["y"] == pytest.approx(400.0, abs=1.0)
    assert cam["z"] == pytest.approx(300.0, abs=1.0)

    # Step 4: Capture screenshot (auto-generated path).
    try:
        data = await call_tool(mcp_client, "capture_screenshot", {})
        assert "path" in data
        assert data.get("file_size_bytes", 0) > 0
        screenshot_path = data["path"]
        assert Path(screenshot_path).exists()
    finally:
        if screenshot_path:
            try:
                os.unlink(screenshot_path)
            except OSError:
                pass

    # Step 5: Change view mode and verify, then restore.
    try:
        await call_tool(mcp_client, "set_viewport_mode", {"mode": "unlit"})
        data = await call_tool(mcp_client, "get_viewport_info", {})
        assert data["view_mode"].lower() == "unlit"
    finally:
        try:
            await call_tool(mcp_client, "set_viewport_mode", {
                "mode": original_mode or "lit",
            })
        except Exception:
            pass
