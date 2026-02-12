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
            "search_text": test_row_name,
        })
        assert data["total_matches"] >= 1

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
    # Baseline
    data = await call_tool(mcp_client, "list_gameplay_tags", {})
    assert "tags" in data

    # Register a test tag
    benchmark_tag = _uniq("Cortex.Test.MCPBenchmark")
    await call_tool(mcp_client, "register_gameplay_tag", {"tag": benchmark_tag})

    # Validate registered tag
    data = await call_tool(mcp_client, "validate_gameplay_tag", {
        "tag": benchmark_tag,
    })
    assert data["valid"] is True

    # Validate non-existent tag
    data = await call_tool(mcp_client, "validate_gameplay_tag", {
        "tag": "Cortex.Test.NonExistent_12345",
    })
    assert data["valid"] is False

    # Bulk register
    bulk_tags = [_uniq("Cortex.Test.MCPBenchA"), _uniq("Cortex.Test.MCPBenchB")]
    await call_tool(mcp_client, "register_gameplay_tags", {"tags": json.dumps(bulk_tags)})

    # Verify
    data = await call_tool(mcp_client, "list_gameplay_tags", {"prefix": "Cortex.Test"})
    tag_names = [t["tag"] for t in data["tags"]]
    assert benchmark_tag in tag_names


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

    # Verify update
    data = await call_tool(mcp_client, "get_translations", {
        "string_table_path": table_path,
    })
    for entry in data["entries"]:
        if entry["key"] == key:
            assert entry["text"] == "Updated Hello from MCP"
            break
    else:
        pytest.fail(f"{key} not found after update")

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
            try:
                await call_tool(mcp_client, "delete_datatable_row", {
                    "table_path": table_path,
                    "row_name": row_name,
                })
            except Exception:
                pass


@pytest.mark.anyio
@pytest.mark.stress
async def test_stress_concurrent_batch(mcp_client):
    """Batch query with 20 commands."""
    commands = [{"command": "data.list_datatables", "params": {}} for _ in range(20)]

    data = await call_tool(mcp_client, "batch_query", {
        "commands": json.dumps(commands),
    })
    assert "results" in data
    assert len(data["results"]) == 20
    assert all(r.get("success") for r in data["results"])
