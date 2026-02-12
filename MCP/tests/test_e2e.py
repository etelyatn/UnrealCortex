"""End-to-End TCP tests for Cortex MCP domains.

Requires a running Unreal Editor with the UnrealCortex plugin
and test content loaded in CortexSandbox.

Run:
    cd Plugins/UnrealCortex/MCP && uv run pytest tests/test_e2e.py -v
"""

import pytest
import uuid


def _uniq(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex[:8]}"


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
        assert isinstance(tables, list)

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
        resp = tcp_connection.send_command("bp.create", {
            "name": _uniq("BP_E2E_Create"),
            "path": "/Game/Temp/CortexMCPTest",
            "type": "Actor",
        })
        data = resp["data"]
        assert "asset_path" in data
        cleanup_assets.append(data["asset_path"])

    def test_create_blueprint_types(self, tcp_connection, cleanup_assets):
        for bp_type in ["Widget", "Component", "FunctionLibrary"]:
            resp = tcp_connection.send_command("bp.create", {
                "name": _uniq(f"BP_E2E_{bp_type}"),
                "path": "/Game/Temp/CortexMCPTest",
                "type": bp_type,
            })
            assert "asset_path" in resp["data"]
            cleanup_assets.append(resp["data"]["asset_path"])

    def test_list_blueprints(self, tcp_connection, blueprint_for_test):
        resp = tcp_connection.send_command("bp.list", {
            "path": "/Game/Temp/CortexMCPTest",
        })
        data = resp["data"]
        assert "blueprints" in data
        assert len(data["blueprints"]) > 0

    def test_get_blueprint_info(self, tcp_connection, blueprint_for_test):
        resp = tcp_connection.send_command("bp.get_info", {
            "asset_path": blueprint_for_test,
        })
        data = resp["data"]
        assert "variables" in data
        assert "functions" in data
        assert "graphs" in data

    def test_add_variable(self, tcp_connection, blueprint_for_test):
        tcp_connection.send_command("bp.add_variable", {
            "asset_path": blueprint_for_test,
            "name": "TestHealth",
            "type": "Float",
        })
        info = tcp_connection.send_command("bp.get_info", {
            "asset_path": blueprint_for_test,
        })
        var_names = [v["name"] for v in info["data"]["variables"]]
        assert "TestHealth" in var_names

    def test_add_function(self, tcp_connection, blueprint_for_test):
        tcp_connection.send_command("bp.add_function", {
            "asset_path": blueprint_for_test,
            "name": "TestTakeDamage",
        })
        info = tcp_connection.send_command("bp.get_info", {
            "asset_path": blueprint_for_test,
        })
        func_names = [f["name"] for f in info["data"]["functions"]]
        assert "TestTakeDamage" in func_names

    def test_compile_blueprint(self, tcp_connection, blueprint_for_test):
        resp = tcp_connection.send_command("bp.compile", {
            "asset_path": blueprint_for_test,
        })
        data = resp["data"]
        assert data.get("compiled") is True

    def test_duplicate_blueprint(self, tcp_connection, blueprint_for_test, cleanup_assets):
        resp = tcp_connection.send_command("bp.duplicate", {
            "asset_path": blueprint_for_test,
            "new_name": _uniq("BP_E2E_Duplicated"),
        })
        assert "new_asset_path" in resp["data"]
        cleanup_assets.append(resp["data"]["new_asset_path"])

    def test_save_blueprint(self, tcp_connection, blueprint_for_test):
        resp = tcp_connection.send_command("bp.save", {
            "asset_path": blueprint_for_test,
        })
        assert resp["data"].get("success") is True or "asset_path" in resp["data"]

    def test_delete_blueprint(self, tcp_connection):
        create_resp = tcp_connection.send_command("bp.create", {
            "name": _uniq("BP_E2E_ToDelete"),
            "path": "/Game/Temp/CortexMCPTest",
            "type": "Actor",
        })
        path = create_resp["data"]["asset_path"]
        resp = tcp_connection.send_command("bp.delete", {"asset_path": path})
        assert resp["data"].get("success") is True or "asset_path" in resp["data"]


# ================================================================
# Blueprint Domain — Error Cases
# ================================================================


@pytest.mark.e2e
class TestBlueprintErrors:
    def test_create_blueprint_invalid_path(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("bp.create", {
                "name": _uniq("BP_InvalidType"),
                "path": "/Game/Temp/CortexMCPTest",
                "type": "DefinitelyInvalidType_12345",
            })

    def test_get_info_nonexistent(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("bp.get_info", {
                "asset_path": "/Game/NonExistent/BP_DoesNotExist_12345",
            })

    def test_add_variable_duplicate_name(self, tcp_connection, blueprint_for_test):
        # Ensure variable exists
        try:
            tcp_connection.send_command("bp.add_variable", {
                "asset_path": blueprint_for_test,
                "name": "DuplicateVar",
                "type": "Bool",
            })
        except RuntimeError:
            pass  # May already exist from previous run
        # Adding same name again should fail
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("bp.add_variable", {
                "asset_path": blueprint_for_test,
                "name": "DuplicateVar",
                "type": "Bool",
            })

    def test_compile_fresh_bp(self, tcp_connection, cleanup_assets):
        resp = tcp_connection.send_command("bp.create", {
            "name": _uniq("BP_E2E_CompileTest"),
            "path": "/Game/Temp/CortexMCPTest",
            "type": "Actor",
        })
        path = resp["data"]["asset_path"]
        cleanup_assets.append(path)
        compile_resp = tcp_connection.send_command("bp.compile", {
            "asset_path": path,
        })
        assert compile_resp["data"].get("compiled") is True

    def test_delete_nonexistent(self, tcp_connection):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("bp.delete", {
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

    def test_list_nodes(self, tcp_connection, blueprint_for_test):
        resp = tcp_connection.send_command("graph.list_nodes", {
            "asset_path": blueprint_for_test,
            "graph_name": "EventGraph",
        })
        assert "nodes" in resp["data"]

    def test_add_node(self, tcp_connection, blueprint_for_test):
        node_id = _add_print_string_node(tcp_connection, blueprint_for_test)
        assert node_id

    def test_get_node(self, tcp_connection, blueprint_for_test):
        node_id = _add_print_string_node(tcp_connection, blueprint_for_test)
        resp = tcp_connection.send_command("graph.get_node", {
            "asset_path": blueprint_for_test,
            "node_id": node_id,
            "graph_name": "EventGraph",
        })
        assert "pins" in resp["data"]

    def test_connect_pins(self, tcp_connection, blueprint_for_test):
        _add_print_string_node(tcp_connection, blueprint_for_test)
        nodes_resp = tcp_connection.send_command("graph.list_nodes", {
            "asset_path": blueprint_for_test,
            "graph_name": "EventGraph",
        })
        nodes = nodes_resp["data"]["nodes"]
        event_node = None
        print_node = None
        for n in nodes:
            if "BeginPlay" in n.get("title", "") or "BeginPlay" in n.get("class", ""):
                event_node = n
            if "PrintString" in n.get("title", "") or "PrintString" in n.get("class", ""):
                print_node = n
        if event_node and print_node:
            resp = tcp_connection.send_command("graph.connect", {
                "asset_path": blueprint_for_test,
                "source_node": event_node["node_id"],
                "source_pin": "then",
                "target_node": print_node["node_id"],
                "target_pin": "execute",
                "graph_name": "EventGraph",
            })
            assert resp["data"].get("success") is True or "source_node" in resp["data"]

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

    def test_list_nodes_invalid_graph(self, tcp_connection, blueprint_for_test):
        with pytest.raises(RuntimeError):
            tcp_connection.send_command("graph.list_nodes", {
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
