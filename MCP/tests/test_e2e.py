"""End-to-End TCP tests for Cortex MCP domains.

Requires a running Unreal Editor with the UnrealCortex plugin
and test content loaded in CortexSandbox.

Run:
    cd Plugins/UnrealCortex/MCP && uv run pytest tests/test_e2e.py -v
"""

import pytest
import uuid
import json


def _uniq(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex[:8]}"


# Permanent test content paths
_COMPLEX_ACTOR_PATH = "/Game/Blueprints/BP_ComplexActor"


def _add_print_string_node(tcp_connection, asset_path: str, graph_name: str = "EventGraph") -> str:
    resp = tcp_connection.send_command("graph.add_node", {
        "asset_path": asset_path,
        "node_class": "UK2Node_CallFunction",
        "graph_name": graph_name,
        "params": {"function_name": "KismetSystemLibrary.PrintString"},
    })
    return resp["data"]["node_id"]


def _ensure_umg_root(tcp_connection, asset_path: str, root_name: str = "TestRootPanel") -> None:
    try:
        tcp_connection.send_command("umg.add_widget", {
            "asset_path": asset_path,
            "widget_class": "CanvasPanel",
            "name": root_name,
        })
    except RuntimeError:
        pass


def _ensure_umg_widget(
    tcp_connection,
    asset_path: str,
    name: str,
    widget_class: str = "TextBlock",
    root_name: str = "TestRootPanel",
) -> None:
    _ensure_umg_root(tcp_connection, asset_path, root_name)
    try:
        tcp_connection.send_command("umg.add_widget", {
            "asset_path": asset_path,
            "widget_class": widget_class,
            "name": name,
            "parent_name": root_name,
        })
    except RuntimeError:
        pass


# ================================================================
# Core Connection
# ================================================================


@pytest.mark.e2e
class TestCoreConnection:
    def test_ping(self, tcp_connection):
        resp = tcp_connection.send_command("ping")
        assert resp["data"]["message"] == "pong"

    def test_get_status(self, tcp_connection):
        resp = tcp_connection.send_command("get_status")
        data = resp["data"]
        assert "plugin_version" in data
        assert "engine_version" in data
        assert "project_name" in data

    def test_get_capabilities(self, tcp_connection):
        resp = tcp_connection.send_command("get_capabilities")
        data = resp["data"]
        assert "domains" in data
        assert isinstance(data["domains"], dict)
        assert "data" in data["domains"]
        data_domain = data["domains"]["data"]
        assert "commands" in data_domain
        assert len(data_domain["commands"]) > 0


@pytest.mark.anyio
@pytest.mark.e2e
async def test_core_and_data_router_tools(mcp_client):
    status = await mcp_client.call_tool("core_cmd", {"command": "get_status"})
    status_data = json.loads(status.content[0].text)
    assert "project_name" in status_data

    tables = await mcp_client.call_tool("data_cmd", {"command": "list_datatables", "params": {}})
    table_data = json.loads(tables.content[0].text)
    assert "datatables" in table_data


# ================================================================
# Data Catalog
# ================================================================


@pytest.mark.e2e
class TestDataCatalog:
    def test_get_data_catalog(self, tcp_connection):
        resp = tcp_connection.send_command("data.get_data_catalog")
        data = resp["data"]
        assert "datatables" in data
        assert "tag_prefixes" in data
        assert "string_tables" in data


# ================================================================
# DataTable Operations
# ================================================================


@pytest.mark.e2e
class TestDataTableOperations:
    def test_list_datatables(self, tcp_connection):
        resp = tcp_connection.send_command("data.list_datatables")
        data = resp["data"]
        assert "datatables" in data
        tables = data["datatables"]
        assert len(tables) > 0
        names = [t["name"] for t in tables]
        assert "DT_TestSimple" in names

    def test_get_datatable_schema(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.get_datatable_schema",
            {"table_path": "/Game/Data/DT_TestSimple"},
        )
        data = resp["data"]
        assert "schema" in data
        schema = data["schema"]
        assert "struct_name" in schema
        assert "fields" in schema
        assert len(schema["fields"]) > 0

    def test_query_datatable(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.query_datatable",
            {"table_path": "/Game/Data/DT_TestSimple", "limit": 10},
        )
        data = resp["data"]
        assert "rows" in data
        assert len(data["rows"]) > 0

    def test_get_datatable_row(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.get_datatable_row",
            {"table_path": "/Game/Data/DT_TestSimple", "row_name": "Sword"},
        )
        data = resp["data"]
        assert data["row_name"] == "Sword"
        assert "row_data" in data

    def test_search_datatable_content(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.search_datatable_content",
            {"table_path": "/Game/Data/DT_TestSimple", "search_text": "Sword"},
        )
        data = resp["data"]
        assert "results" in data
        assert "total_matches" in data

    def test_get_struct_schema(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.get_struct_schema",
            {"struct_name": "GameplayTagTableRow"},
        )
        data = resp["data"]
        assert "schema" in data
        assert "struct_name" in data["schema"]


# ================================================================
# GameplayTag Operations
# ================================================================


@pytest.mark.e2e
class TestGameplayTagOperations:
    def test_list_gameplay_tags(self, tcp_connection):
        resp = tcp_connection.send_command("data.list_gameplay_tags", {})
        data = resp["data"]
        assert "tags" in data
        assert len(data["tags"]) > 0

    def test_validate_tag_valid(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.validate_gameplay_tag", {"tag": "Cortex.Test.Tag1"},
        )
        assert "valid" in resp["data"]

    def test_validate_tag_invalid(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.validate_gameplay_tag",
            {"tag": "NonExistent.Fake.Tag.12345"},
        )
        assert resp["data"]["valid"] is False


# ================================================================
# CurveTable Operations
# ================================================================


@pytest.mark.e2e
class TestCurveTableOperations:
    def test_list_curve_tables(self, tcp_connection):
        resp = tcp_connection.send_command("data.list_curve_tables")
        data = resp["data"]
        assert "curve_tables" in data
        tables = data["curve_tables"]
        assert len(tables) > 0
        names = [t["name"] for t in tables]
        assert "CT_TestCurve" in names

    def test_get_curve_table(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.get_curve_table",
            {"table_path": "/Game/Data/CT_TestCurve"},
        )
        data = resp["data"]
        assert "curves" in data
        assert len(data["curves"]) > 0


# ================================================================
# Localization Operations
# ================================================================


@pytest.mark.e2e
class TestLocalizationOperations:
    def test_list_string_tables(self, tcp_connection):
        resp = tcp_connection.send_command("data.list_string_tables")
        data = resp["data"]
        assert "string_tables" in data
        tables = data["string_tables"]
        assert len(tables) > 0
        names = [t["name"] for t in tables]
        assert "ST_TestStrings" in names

    def test_get_translations(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.get_translations",
            {"string_table_path": "/Game/Data/ST_TestStrings"},
        )
        data = resp["data"]
        assert "entries" in data
        assert len(data["entries"]) >= 4


# ================================================================
# Asset Search
# ================================================================


@pytest.mark.e2e
class TestAssetSearch:
    def test_search_assets(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.search_assets",
            {"path_filter": "/Game/Data/"},
        )
        data = resp["data"]
        assert "assets" in data
        assert len(data["assets"]) > 0


# ================================================================
# DataTable Write Operations
# ================================================================


@pytest.mark.e2e
class TestDataTableWriteOperations:
    """Write tests run in definition order: add -> update -> delete."""

    def test_add_datatable_row(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.add_datatable_row",
            {
                "table_path": "/Game/Data/DT_TestSimple",
                "row_name": "E2E_TestRow",
                "row_data": {"Tag": "Cortex.Test.Tag1"},
            },
        )
        assert resp["data"]["row_name"] == "E2E_TestRow"

    def test_update_datatable_row(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.update_datatable_row",
            {
                "table_path": "/Game/Data/DT_TestSimple",
                "row_name": "E2E_TestRow",
                "row_data": {"Tag": "Cortex.Test.Tag2"},
            },
        )
        assert "row_name" in resp["data"]

    def test_delete_datatable_row(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.delete_datatable_row",
            {
                "table_path": "/Game/Data/DT_TestSimple",
                "row_name": "E2E_TestRow",
            },
        )
        assert resp["data"]["row_name"] == "E2E_TestRow"


# ================================================================
# Edge Cases & Error Handling
# ================================================================


@pytest.mark.e2e
class TestEdgeCases:
    def test_unknown_command(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("data.nonexistent_command_xyz")

    def test_missing_params(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("data.get_datatable_row", {})

    def test_invalid_table_path(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command(
                "data.query_datatable",
                {"table_path": "/Game/NonExistent/FakeTable_12345"},
            )


# ================================================================
# Batch Operations
# ================================================================


@pytest.mark.e2e
class TestBatchOperations:
    def test_batch_query(self, tcp_connection):
        resp = tcp_connection.send_command(
            "batch",
            {
                "commands": [
                    {"command": "get_status", "params": {}},
                    {"command": "data.list_datatables", "params": {}},
                    {"command": "data.list_string_tables", "params": {}},
                ]
            },
        )
        data = resp["data"]
        assert "results" in data
        assert len(data["results"]) == 3
        assert all(r.get("success") for r in data["results"])

# ================================================================
# Blueprint Domain — CRUD Lifecycle
# ================================================================


@pytest.mark.e2e
class TestBlueprintCRUD:
    """Blueprint CRUD tests. Uses blueprint_for_test fixture for read ops."""

    def test_create_blueprint(self, tcp_connection, cleanup_assets):
        resp = tcp_connection.send_command("blueprint.create", {
            "name": _uniq("BP_E2E_Create"),
            "path": "/Game/Temp/CortexMCPTest",
            "type": "Actor",
        })
        data = resp["data"]
        assert "asset_path" in data
        cleanup_assets.append(data["asset_path"])

    def test_create_blueprint_types(self, tcp_connection, cleanup_assets):
        for bp_type in ["Widget", "Component", "FunctionLibrary"]:
            resp = tcp_connection.send_command("blueprint.create", {
                "name": _uniq(f"BP_E2E_{bp_type}"),
                "path": "/Game/Temp/CortexMCPTest",
                "type": bp_type,
            })
            assert "asset_path" in resp["data"]
            cleanup_assets.append(resp["data"]["asset_path"])

    def test_list_blueprints(self, tcp_connection, blueprint_for_test):
        resp = tcp_connection.send_command("blueprint.list", {
            "path": "/Game/Temp/CortexMCPTest",
        })
        data = resp["data"]
        assert "blueprints" in data
        assert len(data["blueprints"]) > 0

    def test_get_blueprint_info(self, tcp_connection, blueprint_for_test):
        resp = tcp_connection.send_command("blueprint.get_info", {
            "asset_path": blueprint_for_test,
        })
        data = resp["data"]
        assert "variables" in data
        assert "functions" in data
        assert "graphs" in data

    def test_add_variable(self, tcp_connection, blueprint_for_test):
        tcp_connection.send_command("blueprint.add_variable", {
            "asset_path": blueprint_for_test,
            "name": "TestHealth",
            "type": "Float",
        })
        info = tcp_connection.send_command("blueprint.get_info", {
            "asset_path": blueprint_for_test,
        })
        var_names = [v["name"] for v in info["data"]["variables"]]
        assert "TestHealth" in var_names

    def test_add_function(self, tcp_connection, blueprint_for_test):
        tcp_connection.send_command("blueprint.add_function", {
            "asset_path": blueprint_for_test,
            "name": "TestTakeDamage",
        })
        info = tcp_connection.send_command("blueprint.get_info", {
            "asset_path": blueprint_for_test,
        })
        func_names = [f["name"] for f in info["data"]["functions"]]
        assert "TestTakeDamage" in func_names

    def test_compile_blueprint(self, tcp_connection, blueprint_for_test):
        resp = tcp_connection.send_command("blueprint.compile", {
            "asset_path": blueprint_for_test,
        })
        data = resp["data"]
        assert data["compile_status"] in {"success", "warning"}
        assert data["error_count"] == 0
        assert "warning_count" in data
        assert "diagnostics" in data
        assert isinstance(data["diagnostics"], list)

    def test_duplicate_blueprint(self, tcp_connection, blueprint_for_test, cleanup_assets):
        resp = tcp_connection.send_command("blueprint.duplicate", {
            "asset_path": blueprint_for_test,
            "new_name": _uniq("BP_E2E_Duplicated"),
        })
        assert "new_asset_path" in resp["data"]
        cleanup_assets.append(resp["data"]["new_asset_path"])

    def test_save_blueprint(self, tcp_connection, blueprint_for_test):
        resp = tcp_connection.send_command("blueprint.save", {
            "asset_path": blueprint_for_test,
        })
        assert resp["data"].get("success") is True or "asset_path" in resp["data"]

    def test_delete_blueprint(self, tcp_connection):
        create_resp = tcp_connection.send_command("blueprint.create", {
            "name": _uniq("BP_E2E_ToDelete"),
            "path": "/Game/Temp/CortexMCPTest",
            "type": "Actor",
        })
        path = create_resp["data"]["asset_path"]
        resp = tcp_connection.send_command("blueprint.delete", {"asset_path": path})
        assert resp["data"].get("success") is True or "asset_path" in resp["data"]


# ================================================================
# Blueprint Domain — Class Defaults (CDO)
# ================================================================


@pytest.mark.e2e
class TestBlueprintClassDefaults:
    """Blueprint Class Default Object property tests."""

    def test_get_class_defaults_discovery(self, tcp_connection, blueprint_for_test):
        """Discovery mode: get all settable properties."""
        resp = tcp_connection.send_command("blueprint.get_class_defaults", {
            "blueprint_path": blueprint_for_test,
        })
        data = resp["data"]
        assert "properties" in data
        assert "class" in data
        assert "parent_class" in data
        assert data["count"] > 0

    def test_get_class_defaults_specific(self, tcp_connection, blueprint_for_test):
        """Read specific CDO properties by name."""
        resp = tcp_connection.send_command("blueprint.get_class_defaults", {
            "blueprint_path": blueprint_for_test,
            "properties": ["PrimaryActorTick.bCanEverTick", "bReplicates"],
        })
        data = resp["data"]
        props = data["properties"]
        assert "PrimaryActorTick.bCanEverTick" in props
        assert "bReplicates" in props
        assert "type" in props["PrimaryActorTick.bCanEverTick"]
        assert "value" in props["PrimaryActorTick.bCanEverTick"]

    def test_set_class_defaults_bool(self, tcp_connection, blueprint_for_test):
        """Set a bool CDO property and verify via get."""
        tcp_connection.send_command("blueprint.set_class_defaults", {
            "blueprint_path": blueprint_for_test,
            "properties": {"PrimaryActorTick.bCanEverTick": True},
            "compile": False,
            "save": False,
        })
        resp = tcp_connection.send_command("blueprint.get_class_defaults", {
            "blueprint_path": blueprint_for_test,
            "properties": ["PrimaryActorTick.bCanEverTick"],
        })
        val = resp["data"]["properties"]["PrimaryActorTick.bCanEverTick"]["value"]
        assert val is True

    def test_set_class_defaults_batch(self, tcp_connection, blueprint_for_test):
        """Set multiple CDO properties in one call."""
        resp = tcp_connection.send_command("blueprint.set_class_defaults", {
            "blueprint_path": blueprint_for_test,
            "properties": {"PrimaryActorTick.bCanEverTick": True, "bReplicates": True},
            "compile": False,
            "save": False,
        })
        data = resp["data"]
        assert "results" in data
        assert "PrimaryActorTick.bCanEverTick" in data["results"]
        assert "bReplicates" in data["results"]

    def test_set_class_defaults_no_compile(self, tcp_connection, blueprint_for_test):
        """compile=false should skip compilation."""
        resp = tcp_connection.send_command("blueprint.set_class_defaults", {
            "blueprint_path": blueprint_for_test,
            "properties": {"PrimaryActorTick.bCanEverTick": False},
            "compile": False,
            "save": False,
        })
        data = resp["data"]
        assert data["compiled"] is False
        assert data["saved"] is False

    def test_get_class_defaults_property_not_found(self, tcp_connection, blueprint_for_test):
        """Misspelled property returns error with fuzzy suggestions."""
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("blueprint.get_class_defaults", {
                "blueprint_path": blueprint_for_test,
                "properties": ["bCanEvrTick"],
            })

    def test_set_class_defaults_property_not_found(self, tcp_connection, blueprint_for_test):
        """Non-existent property returns error."""
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("blueprint.set_class_defaults", {
                "blueprint_path": blueprint_for_test,
                "properties": {"NonExistentProperty_12345": "value"},
            })

    def test_get_class_defaults_nonexistent_bp(self, tcp_connection):
        """Non-existent Blueprint returns BLUEPRINT_NOT_FOUND."""
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("blueprint.get_class_defaults", {
                "blueprint_path": "/Game/NonExistent/BP_Ghost_12345",
            })


# ================================================================
# Blueprint Domain — Error Cases
# ================================================================


@pytest.mark.e2e
class TestBlueprintErrors:
    def test_create_blueprint_invalid_path(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("blueprint.create", {
                "name": _uniq("BP_InvalidType"),
                "path": "/Game/Temp/CortexMCPTest",
                "type": "DefinitelyInvalidType_12345",
            })

    def test_get_info_nonexistent(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("blueprint.get_info", {
                "asset_path": "/Game/NonExistent/BP_DoesNotExist_12345",
            })

    def test_add_variable_duplicate_name(self, tcp_connection, blueprint_for_test):
        # Ensure variable exists
        try:
            tcp_connection.send_command("blueprint.add_variable", {
                "asset_path": blueprint_for_test,
                "name": "DuplicateVar",
                "type": "Bool",
            })
        except RuntimeError:
            pass  # May already exist from previous run
        # Adding same name again should fail
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("blueprint.add_variable", {
                "asset_path": blueprint_for_test,
                "name": "DuplicateVar",
                "type": "Bool",
            })

    def test_compile_fresh_bp(self, tcp_connection, cleanup_assets):
        resp = tcp_connection.send_command("blueprint.create", {
            "name": _uniq("BP_E2E_CompileTest"),
            "path": "/Game/Temp/CortexMCPTest",
            "type": "Actor",
        })
        path = resp["data"]["asset_path"]
        cleanup_assets.append(path)
        compile_resp = tcp_connection.send_command("blueprint.compile", {
            "asset_path": path,
        })
        assert compile_resp["data"]["compile_status"] == "success"
        assert compile_resp["data"]["error_count"] == 0
        assert isinstance(compile_resp["data"]["diagnostics"], list)

    def test_delete_nonexistent(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("blueprint.delete", {
                "asset_path": "/Game/NonExistent/BP_Ghost_12345",
            })

# ================================================================
# Graph Domain — CRUD Lifecycle
# ================================================================


@pytest.mark.e2e
class TestGraphCRUD:
    """Graph node/connection tests. Uses blueprint_for_test as host BP."""

    def test_list_graphs(self, tcp_connection, blueprint_for_test):
        resp = tcp_connection.send_command("graph.list_graphs", {
            "asset_path": blueprint_for_test,
        })
        data = resp["data"]
        assert "graphs" in data
        graph_names = [g["name"] for g in data["graphs"]]
        assert "EventGraph" in graph_names

    def test_get_subgraph(self, tcp_connection, blueprint_for_test):
        resp = tcp_connection.send_command("graph.get_subgraph", {
            "asset_path": blueprint_for_test,
            "graph_name": "EventGraph",
        })
        assert "nodes" in resp["data"]

    def test_add_node(self, tcp_connection, blueprint_for_test):
        node_id = _add_print_string_node(tcp_connection, blueprint_for_test)
        assert node_id

    def test_trace_exec(self, tcp_connection, blueprint_for_test):
        node_id = _add_print_string_node(tcp_connection, blueprint_for_test)
        resp = tcp_connection.send_command("graph.trace_exec", {
            "asset_path": blueprint_for_test,
            "start_node_id": node_id,
            "include_edges": True,
        })
        assert "nodes" in resp["data"]

    def test_connect_pins(self, tcp_connection, blueprint_for_test):
        _add_print_string_node(tcp_connection, blueprint_for_test)
        nodes_resp = tcp_connection.send_command("graph.get_subgraph", {
            "asset_path": blueprint_for_test,
            "graph_name": "EventGraph",
        })
        nodes = nodes_resp["data"]["nodes"]
        event_node = None
        print_node = None
        for n in nodes:
            label = n.get("display_name", "") or n.get("title", "")
            if "BeginPlay" in label:
                event_node = n
            if "Print String" in label or "PrintString" in label:
                print_node = n
        assert event_node, "BeginPlay node not found in EventGraph"
        assert print_node, "PrintString node not found in EventGraph"
        resp = tcp_connection.send_command("graph.connect", {
            "asset_path": blueprint_for_test,
            "source_node": event_node["node_id"],
            "source_pin": "then",
            "target_node": print_node["node_id"],
            "target_pin": "execute",
            "graph_name": "EventGraph",
        })
        assert resp["data"].get("connected") is True

    def test_disconnect_pins(self, tcp_connection, blueprint_for_test):
        node_id = _add_print_string_node(tcp_connection, blueprint_for_test)
        resp = tcp_connection.send_command("graph.disconnect", {
            "asset_path": blueprint_for_test,
            "node_id": node_id,
            "pin_name": "then",
            "graph_name": "EventGraph",
        })
        assert resp["data"].get("disconnected") is True

    def test_remove_node(self, tcp_connection, blueprint_for_test):
        node_id = _add_print_string_node(tcp_connection, blueprint_for_test)
        resp = tcp_connection.send_command("graph.remove_node", {
            "asset_path": blueprint_for_test,
            "node_id": node_id,
            "graph_name": "EventGraph",
        })
        assert "removed_node_id" in resp["data"]


# ================================================================
# Graph Domain — Error Cases
# ================================================================


@pytest.mark.e2e
class TestGraphErrors:
    def test_add_node_invalid_class(self, tcp_connection, blueprint_for_test):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("graph.add_node", {
                "asset_path": blueprint_for_test,
                "node_class": "NonExistent.FakeNode.12345",
                "graph_name": "EventGraph",
            })

    def test_connect_incompatible_pins(self, tcp_connection, blueprint_for_test):
        node1 = _add_print_string_node(tcp_connection, blueprint_for_test)
        node2 = _add_print_string_node(tcp_connection, blueprint_for_test)
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("graph.connect", {
                "asset_path": blueprint_for_test,
                "source_node": node1,
                "source_pin": "execute",  # exec pin
                "target_node": node2,
                "target_pin": "InString",  # string pin — type mismatch
                "graph_name": "EventGraph",
            })

    def test_remove_nonexistent_node(self, tcp_connection, blueprint_for_test):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("graph.remove_node", {
                "asset_path": blueprint_for_test,
                "node_id": "NonExistentNodeId_12345",
                "graph_name": "EventGraph",
            })

    def test_get_subgraph_invalid_graph(self, tcp_connection, blueprint_for_test):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("graph.get_subgraph", {
                "asset_path": blueprint_for_test,
                "graph_name": "NonExistentGraph_12345",
            })

# ================================================================
# UMG Domain — CRUD Lifecycle
# ================================================================


@pytest.mark.e2e
class TestUMGCRUD:
    """Widget CRUD tests. Uses widget_bp_for_test fixture."""

    def test_list_widget_classes(self, tcp_connection):
        resp = tcp_connection.send_command("umg.list_widget_classes", {})
        data = resp["data"]
        classes = data.get("classes") or data.get("widget_classes", [])
        assert len(classes) > 0

    def test_add_widget(self, tcp_connection, widget_bp_for_test):
        resp = tcp_connection.send_command("umg.add_widget", {
            "asset_path": widget_bp_for_test,
            "widget_class": "CanvasPanel",
            "name": "TestRootPanel",
        })
        assert resp["data"].get("success") is True or "name" in resp["data"]

    def test_get_tree(self, tcp_connection, widget_bp_for_test):
        try:
            tcp_connection.send_command("umg.add_widget", {
                "asset_path": widget_bp_for_test,
                "widget_class": "CanvasPanel",
                "name": "TreeTestPanel",
            })
        except RuntimeError:
            pass
        resp = tcp_connection.send_command("umg.get_tree", {
            "asset_path": widget_bp_for_test,
        })
        data = resp["data"]
        assert "tree" in data or "root" in data or "widgets" in data

    def test_get_widget(self, tcp_connection, widget_bp_for_test):
        _ensure_umg_widget(tcp_connection, widget_bp_for_test, "TestTextWidget")
        resp = tcp_connection.send_command("umg.get_widget", {
            "asset_path": widget_bp_for_test,
            "widget_name": "TestTextWidget",
        })
        assert "name" in resp["data"] or "widget_name" in resp["data"]

    def test_set_text(self, tcp_connection, widget_bp_for_test):
        _ensure_umg_widget(tcp_connection, widget_bp_for_test, "SetTextTarget")
        resp = tcp_connection.send_command("umg.set_text", {
            "asset_path": widget_bp_for_test,
            "widget_name": "SetTextTarget",
            "text": "Hello E2E",
        })
        assert resp["data"].get("success") is True or "widget_name" in resp["data"]

    def test_set_color(self, tcp_connection, widget_bp_for_test):
        _ensure_umg_widget(tcp_connection, widget_bp_for_test, "SetColorTarget")
        resp = tcp_connection.send_command("umg.set_color", {
            "asset_path": widget_bp_for_test,
            "widget_name": "SetColorTarget",
            "color": "#FF0000",
        })
        assert resp["data"].get("success") is True or "widget_name" in resp["data"]

    def test_set_visibility(self, tcp_connection, widget_bp_for_test):
        _ensure_umg_widget(tcp_connection, widget_bp_for_test, "VisibilityTarget")
        resp = tcp_connection.send_command("umg.set_visibility", {
            "asset_path": widget_bp_for_test,
            "widget_name": "VisibilityTarget",
            "visibility": "Collapsed",
        })
        assert resp["data"].get("success") is True or "widget_name" in resp["data"]

    def test_set_property(self, tcp_connection, widget_bp_for_test):
        _ensure_umg_widget(tcp_connection, widget_bp_for_test, "PropTarget")
        resp = tcp_connection.send_command("umg.set_property", {
            "asset_path": widget_bp_for_test,
            "widget_name": "PropTarget",
            "property_path": "Text",
            "value": '"E2E Property Test"',
        })
        assert resp["data"].get("success") is True or "widget_name" in resp["data"]

    def test_get_property(self, tcp_connection, widget_bp_for_test):
        _ensure_umg_widget(tcp_connection, widget_bp_for_test, "GetPropTarget")
        resp = tcp_connection.send_command("umg.get_property", {
            "asset_path": widget_bp_for_test,
            "widget_name": "GetPropTarget",
            "property_path": "Text",
        })
        assert "value" in resp["data"] or "property_path" in resp["data"]

    def test_get_schema(self, tcp_connection, widget_bp_for_test):
        _ensure_umg_widget(tcp_connection, widget_bp_for_test, "SchemaTarget")
        resp = tcp_connection.send_command("umg.get_schema", {
            "asset_path": widget_bp_for_test,
            "widget_name": "SchemaTarget",
        })
        assert "properties" in resp["data"] or "schema" in resp["data"]

    def test_create_animation(self, tcp_connection, widget_bp_for_test):
        resp = tcp_connection.send_command("umg.create_animation", {
            "asset_path": widget_bp_for_test,
            "animation_name": "TestFadeIn",
            "length": 1.0,
        })
        assert resp["data"].get("success") is True or "animation_name" in resp["data"]

    def test_list_animations(self, tcp_connection, widget_bp_for_test):
        resp = tcp_connection.send_command("umg.list_animations", {
            "asset_path": widget_bp_for_test,
        })
        assert "animations" in resp["data"]

    def test_duplicate_widget(self, tcp_connection, widget_bp_for_test):
        _ensure_umg_widget(tcp_connection, widget_bp_for_test, "DupSource")
        resp = tcp_connection.send_command("umg.duplicate_widget", {
            "asset_path": widget_bp_for_test,
            "widget_name": "DupSource",
            "new_name": "DupCopy",
        })
        assert resp["data"].get("duplicated") is True


# ================================================================
# UMG Domain — Error Cases
# ================================================================


@pytest.mark.e2e
class TestUMGErrors:
    def test_add_widget_invalid_class(self, tcp_connection, widget_bp_for_test):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("umg.add_widget", {
                "asset_path": widget_bp_for_test,
                "widget_class": "NonExistentWidget_12345",
                "name": "BadWidget",
            })

    def test_set_property_nonexistent_widget(self, tcp_connection, widget_bp_for_test):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("umg.set_property", {
                "asset_path": widget_bp_for_test,
                "widget_name": "NonExistentWidget_12345",
                "property_path": "Text",
                "value": '"hello"',
            })

    def test_reparent_invalid_parent(self, tcp_connection, widget_bp_for_test):
        try:
            tcp_connection.send_command("umg.add_widget", {
                "asset_path": widget_bp_for_test,
                "widget_class": "TextBlock",
                "name": "ReparentOrphan",
            })
        except RuntimeError:
            pass
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("umg.reparent", {
                "asset_path": widget_bp_for_test,
                "widget_name": "ReparentOrphan",
                "new_parent": "NonExistentParent_12345",
            })

    def test_set_color_invalid_format(self, tcp_connection, widget_bp_for_test):
        try:
            tcp_connection.send_command("umg.add_widget", {
                "asset_path": widget_bp_for_test,
                "widget_class": "TextBlock",
                "name": "BadColorTarget",
            })
        except RuntimeError:
            pass
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("umg.set_color", {
                "asset_path": widget_bp_for_test,
                "widget_name": "BadColorTarget",
                "color": "not-a-color",
            })

# ================================================================
# Material Domain — Property Setters
# ================================================================


@pytest.mark.e2e
class TestMaterialPropertySetters:
    """Test set_material_property and set_material_node_property via TCP."""

    def _create_temp_material(self, tcp_connection, prefix="M_E2E"):
        name = _uniq(prefix)
        resp = tcp_connection.send_command("material.create_material", {
            "asset_path": "/Game/Temp/CortexMCPTest",
            "name": name,
        })
        return resp["data"]["asset_path"]

    def _delete_material(self, tcp_connection, asset_path):
        try:
            tcp_connection.send_command("material.delete_material", {
                "asset_path": asset_path,
            })
        except (RuntimeError, ConnectionError):
            pass

    def test_set_material_domain_post_process(self, tcp_connection):
        """Set MaterialDomain to MD_PostProcess and verify."""
        path = self._create_temp_material(tcp_connection, "M_E2E_Domain")
        try:
            resp = tcp_connection.send_command("material.set_material_property", {
                "asset_path": path,
                "property_name": "MaterialDomain",
                "value": "MD_PostProcess",
            })
            assert resp["data"]["updated"] is True

            # Verify via get_material
            resp = tcp_connection.send_command("material.get_material", {
                "asset_path": path,
            })
            assert resp["data"]["material_domain"] == "PostProcess"
        finally:
            self._delete_material(tcp_connection, path)

    def test_set_material_blend_mode(self, tcp_connection):
        """Set BlendMode to BLEND_Masked and verify."""
        path = self._create_temp_material(tcp_connection, "M_E2E_Blend")
        try:
            resp = tcp_connection.send_command("material.set_material_property", {
                "asset_path": path,
                "property_name": "BlendMode",
                "value": "BLEND_Masked",
            })
            assert resp["data"]["updated"] is True

            resp = tcp_connection.send_command("material.get_material", {
                "asset_path": path,
            })
            assert resp["data"]["blend_mode"] == "Masked"
        finally:
            self._delete_material(tcp_connection, path)

    def test_set_material_bool_property(self, tcp_connection):
        """Set TwoSided bool property."""
        path = self._create_temp_material(tcp_connection, "M_E2E_Bool")
        try:
            resp = tcp_connection.send_command("material.set_material_property", {
                "asset_path": path,
                "property_name": "TwoSided",
                "value": True,
            })
            assert resp["data"]["updated"] is True

            resp = tcp_connection.send_command("material.get_material", {
                "asset_path": path,
            })
            assert resp["data"]["two_sided"] is True
        finally:
            self._delete_material(tcp_connection, path)

    def test_set_material_property_invalid(self, tcp_connection):
        """Invalid property name returns error."""
        path = self._create_temp_material(tcp_connection, "M_E2E_Invalid")
        try:
            with pytest.raises(RuntimeError, match="not found"):
                tcp_connection.send_command("material.set_material_property", {
                    "asset_path": path,
                    "property_name": "NonExistentProp",
                    "value": "whatever",
                })
        finally:
            self._delete_material(tcp_connection, path)

    def test_set_node_property_byte_enum(self, tcp_connection):
        """Set SceneTextureId (FByteProperty enum) on SceneTexture node."""
        path = self._create_temp_material(tcp_connection, "M_E2E_Enum")
        try:
            # Add SceneTexture node
            resp = tcp_connection.send_command("material.add_node", {
                "asset_path": path,
                "expression_class": "MaterialExpressionSceneTexture",
            })
            node_id = resp["data"]["node_id"]

            # Set SceneTextureId
            resp = tcp_connection.send_command("material.set_node_property", {
                "asset_path": path,
                "node_id": node_id,
                "property_name": "SceneTextureId",
                "value": "PPI_PostProcessInput0",
            })
            assert resp["data"]["updated"] is True

            # Verify via get_node
            resp = tcp_connection.send_command("material.get_node", {
                "asset_path": path,
                "node_id": node_id,
            })
            props = resp["data"].get("properties", {})
            assert props.get("SceneTextureId") == "PPI_PostProcessInput0"
        finally:
            self._delete_material(tcp_connection, path)

    def test_set_node_property_invalid_enum(self, tcp_connection):
        """Invalid enum value returns error with valid values listed."""
        path = self._create_temp_material(tcp_connection, "M_E2E_BadEnum")
        try:
            resp = tcp_connection.send_command("material.add_node", {
                "asset_path": path,
                "expression_class": "MaterialExpressionSceneTexture",
            })
            node_id = resp["data"]["node_id"]

            with pytest.raises(RuntimeError, match="Valid"):
                tcp_connection.send_command("material.set_node_property", {
                    "asset_path": path,
                    "node_id": node_id,
                    "property_name": "SceneTextureId",
                    "value": "InvalidEnumValue",
                })
        finally:
            self._delete_material(tcp_connection, path)


# ================================================================
# Data Domain — Expanded Error/Edge Cases
# ================================================================


@pytest.mark.e2e
class TestDataExpanded:
    def test_update_nonexistent_row(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command(
                "data.update_datatable_row",
                {
                    "table_path": "/Game/Data/DT_TestSimple",
                    "row_name": "NonExistentRow_12345",
                    "row_data": {"Tag": "Cortex.Test.Tag1"},
                },
            )

    def test_import_malformed_json(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command(
                "data.import_datatable_json",
                {
                    "table_path": "/Game/Data/DT_TestSimple",
                    "rows": "not valid json{{{",
                    "mode": "create",
                },
            )

    def test_batch_mixed_valid_invalid(self, tcp_connection):
        resp = tcp_connection.send_command(
            "batch",
            {
                "commands": [
                    {"command": "get_status", "params": {}},
                    {
                        "command": "data.query_datatable",
                        "params": {"table_path": "/Game/NonExistent/Fake_12345"},
                    },
                    {"command": "data.list_string_tables", "params": {}},
                ]
            },
        )
        results = resp["data"]["results"]
        assert len(results) == 3
        assert results[0].get("success") is True
        assert results[1].get("success") is False
        assert results[2].get("success") is True

    def test_search_empty_results(self, tcp_connection):
        resp = tcp_connection.send_command(
            "data.search_datatable_content",
            {
                "table_path": "/Game/Data/DT_TestSimple",
                "search_text": "ThisStringDefinitelyDoesNotExistAnywhere_12345",
            },
        )
        data = resp["data"]
        assert data["total_matches"] == 0
        assert len(data["results"]) == 0


# ================================================================
# Blueprint Domain — Migration Analysis
# ================================================================


@pytest.mark.e2e
class TestBlueprintAnalysis:
    """Tests for bp.analyze_for_migration against BP_ComplexActor."""

    def test_analyze_for_migration(self, tcp_connection):
        resp = tcp_connection.send_command(
            "blueprint.analyze_for_migration",
            {"asset_path": _COMPLEX_ACTOR_PATH},
        )
        data = resp["data"]
        assert "variables" in data
        assert "timelines" in data
        assert "event_dispatchers" in data
        assert "latent_nodes" in data
        assert "complexity_metrics" in data
        assert "graphs" in data
        assert "interfaces_implemented" in data
        assert isinstance(data["variables"], list)
        assert isinstance(data["timelines"], list)
        assert isinstance(data["event_dispatchers"], list)
        assert isinstance(data["latent_nodes"], list)
        assert isinstance(data["complexity_metrics"], dict)
        assert isinstance(data["graphs"], list)
        assert isinstance(data["interfaces_implemented"], list)

    def test_analyze_for_migration_not_found(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command(
                "blueprint.analyze_for_migration",
                {"asset_path": "/Game/NonExistent/BP_Ghost_12345"},
            )

    def test_analyze_for_migration_variable_schema(self, tcp_connection):
        resp = tcp_connection.send_command(
            "blueprint.analyze_for_migration",
            {"asset_path": _COMPLEX_ACTOR_PATH},
        )
        variables = resp["data"]["variables"]
        assert len(variables) > 0, f"BP_ComplexActor returned no variables: {resp['data']}"
        for var in variables:
            # is_replicated is the flat top-level bool (not the nested "replication" object)
            assert "is_replicated" in var
            assert "container_type" in var
            assert "usage_count" in var

    def test_analyze_for_migration_complexity_metrics(self, tcp_connection):
        resp = tcp_connection.send_command(
            "blueprint.analyze_for_migration",
            {"asset_path": _COMPLEX_ACTOR_PATH},
        )
        metrics = resp["data"]["complexity_metrics"]
        assert metrics["total_nodes"] > 0, f"BP_ComplexActor total_nodes should be > 0: {metrics}"
        assert isinstance(metrics["total_connections"], int) and metrics["total_connections"] >= 0
        assert metrics["migration_confidence"] in {"high", "medium", "low"}


# ================================================================
# Blueprint Domain — Migration Asset Fixtures
# ================================================================

_MIGRATION_PATH = "/Game/Blueprints/Migration"
_BP_TIMELINE_TEST = f"{_MIGRATION_PATH}/BP_TimelineTest"
_BP_DISPATCHER_TEST = f"{_MIGRATION_PATH}/BP_DispatcherTest"
_BP_LATENT_TEST = f"{_MIGRATION_PATH}/BP_LatentTest"
_BP_INTERFACE_TEST = f"{_MIGRATION_PATH}/BP_InterfaceTest"
_BP_COMPONENT_MIGRATE = f"{_MIGRATION_PATH}/BP_ComponentMigrate"
_BP_FUNC_LIB_MIGRATE = f"{_MIGRATION_PATH}/BP_FuncLibMigrate"
_BPI_MIGRATION_TEST = f"{_MIGRATION_PATH}/BPI_MigrationTest"


@pytest.mark.e2e
class TestMigrationAssets:
    """Tests for analyze_for_migration against purpose-built migration fixture BPs."""

    def test_aaa_migration_assets_all_exist(self, tcp_connection):
        """Smoke guard — runs first so missing fixtures produce a clear error, not cryptic assertion failures."""
        paths = [
            _BPI_MIGRATION_TEST,
            _BP_TIMELINE_TEST,
            _BP_DISPATCHER_TEST,
            _BP_LATENT_TEST,
            _BP_INTERFACE_TEST,
            _BP_COMPONENT_MIGRATE,
            _BP_FUNC_LIB_MIGRATE,
        ]
        for path in paths:
            # send_command raises RuntimeError on failure — no need for success assertion
            tcp_connection.send_command("blueprint.get_info", {"asset_path": path})

    def test_timeline_bp_detects_timeline(self, tcp_connection):
        resp = tcp_connection.send_command(
            "blueprint.analyze_for_migration",
            {"asset_path": _BP_TIMELINE_TEST},
        )
        data = resp["data"]
        assert len(data["timelines"]) >= 1, "BP_TimelineTest should have at least one timeline"
        timeline = data["timelines"][0]
        assert "float_tracks" in timeline
        assert "auto_play" in timeline
        assert "loop" in timeline

    def test_dispatcher_bp_detects_event_dispatcher(self, tcp_connection):
        resp = tcp_connection.send_command(
            "blueprint.analyze_for_migration",
            {"asset_path": _BP_DISPATCHER_TEST},
        )
        data = resp["data"]
        dispatchers = data["event_dispatchers"]
        assert len(dispatchers) >= 1, "BP_DispatcherTest should have OnHealthChanged dispatcher"

        # Name-based lookup — robust to ordering changes or future additional dispatchers
        dispatcher = next((d for d in dispatchers if d.get("name") == "OnHealthChanged"), None)
        assert dispatcher is not None, f"OnHealthChanged not found in: {[d.get('name') for d in dispatchers]}"
        assert isinstance(dispatcher["params"], list)

    def test_dispatcher_variables_consistent_with_event_dispatchers(self, tcp_connection):
        """Dispatcher names in event_dispatchers must also appear in variables with type='dispatcher'."""
        resp = tcp_connection.send_command(
            "blueprint.analyze_for_migration",
            {"asset_path": _BP_DISPATCHER_TEST},
        )
        data = resp["data"]
        dispatcher_names = {d["name"] for d in data["event_dispatchers"] if "name" in d}
        variable_dispatcher_names = {
            v["name"] for v in data["variables"]
            if v.get("type") == "dispatcher"
        }
        assert dispatcher_names == variable_dispatcher_names, (
            f"event_dispatchers names {dispatcher_names} should match "
            f"variables with type='dispatcher' {variable_dispatcher_names}"
        )

    def test_latent_bp_detects_latent_nodes(self, tcp_connection):
        resp = tcp_connection.send_command(
            "blueprint.analyze_for_migration",
            {"asset_path": _BP_LATENT_TEST},
        )
        data = resp["data"]
        assert len(data["latent_nodes"]) >= 1, "BP_LatentTest should have at least one latent node (Delay)"
        latent = data["latent_nodes"][0]
        assert "is_sequential" in latent
        assert "sequence_length" in latent
        # duration_pin_value is only emitted when non-empty; BP_LatentTest sets Duration=2.0
        assert "duration_pin_value" in latent, "BP_LatentTest Delay node should have a duration default value"

    def test_interface_bp_detects_implemented_interfaces(self, tcp_connection):
        resp = tcp_connection.send_command(
            "blueprint.analyze_for_migration",
            {"asset_path": _BP_INTERFACE_TEST},
        )
        data = resp["data"]
        interfaces = data["interfaces_implemented"]
        assert len(interfaces) >= 1, "BP_InterfaceTest should implement BPI_MigrationTest"
        iface_names = [i["name"] for i in interfaces if "name" in i]
        assert any("MigrationTest" in name for name in iface_names), (
            f"Expected BPI_MigrationTest in interfaces: {interfaces}"
        )

    def test_component_bp_variable_schema(self, tcp_connection):
        resp = tcp_connection.send_command(
            "blueprint.analyze_for_migration",
            {"asset_path": _BP_COMPONENT_MIGRATE},
        )
        data = resp["data"]
        variables = data["variables"]
        assert len(variables) >= 2, "BP_ComponentMigrate should have TickCount and bCustomIsActive"
        var_names = {v["name"] for v in variables}
        assert "TickCount" in var_names
        assert "bCustomIsActive" in var_names
        for var in variables:
            assert "is_replicated" in var
            assert "container_type" in var
            assert "usage_count" in var

    def test_function_library_bp_functions(self, tcp_connection):
        resp = tcp_connection.send_command(
            "blueprint.analyze_for_migration",
            {"asset_path": _BP_FUNC_LIB_MIGRATE},
        )
        data = resp["data"]
        func_names = {f["name"] for f in data.get("functions", [])}
        assert "CalculateValue" in func_names
        assert "IsValidTarget" in func_names
        assert "FormatLabel" in func_names
        metrics = data["complexity_metrics"]
        assert metrics["total_nodes"] >= 0
        assert metrics["migration_confidence"] in {"high", "medium", "low"}
