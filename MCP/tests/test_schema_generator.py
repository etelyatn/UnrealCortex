"""Unit tests for schema_generator module."""

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from cortex_mcp.schema_generator import find_project_root, get_schema_dir
from cortex_mcp.schema_generator import render_data_schema, SCHEMA_VERSION, render_catalog
from cortex_mcp.schema_generator import collect_data_domain
from cortex_mcp.schema_generator import generate_schema
from cortex_mcp.schema_generator import read_meta_from_file
from cortex_mcp.schema_generator import _decode_data


class TestProjectRootDiscovery(unittest.TestCase):

    def test_env_var_override(self):
        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": "/fake/project"}):
            result = find_project_root()
            self.assertEqual(result, Path("/fake/project"))

    def test_uproject_walk_up(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            # Create a fake .uproject file
            (Path(tmpdir) / "Test.uproject").touch()
            # Simulate being called from a nested path
            nested = Path(tmpdir) / "Plugins" / "MCP" / "src"
            nested.mkdir(parents=True)

            with patch("cortex_mcp.schema_generator._get_caller_path", return_value=nested):
                result = find_project_root()
                self.assertEqual(result, Path(tmpdir))

    def test_schema_dir_path(self):
        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": "/fake/project"}):
            result = get_schema_dir()
            self.assertEqual(result, Path("/fake/project/.cortex/schema"))


class TestAtomicWrite(unittest.TestCase):

    def test_atomic_write_creates_file(self):
        from cortex_mcp.schema_generator import atomic_write

        with tempfile.TemporaryDirectory() as tmpdir:
            target = Path(tmpdir) / "test.md"
            atomic_write(target, "# Hello\n\nContent here.")
            self.assertTrue(target.exists())
            self.assertEqual(target.read_text(encoding="utf-8"), "# Hello\n\nContent here.")

    def test_atomic_write_no_partial_on_error(self):
        from cortex_mcp.schema_generator import atomic_write
        from unittest.mock import patch

        with tempfile.TemporaryDirectory() as tmpdir:
            target = Path(tmpdir) / "test.md"
            target.write_text("original", encoding="utf-8")

            # Force os.replace to fail — simulates disk-full or permission error
            with patch("os.replace", side_effect=OSError("simulated failure")):
                with self.assertRaises(OSError):
                    atomic_write(target, "new content")

            # Original file should be unchanged
            self.assertEqual(target.read_text(encoding="utf-8"), "original")
            # No .tmp files left behind
            tmp_files = list(Path(tmpdir).glob("*.tmp"))
            self.assertEqual(len(tmp_files), 0)

    def test_atomic_write_creates_parent_dirs(self):
        from cortex_mcp.schema_generator import atomic_write

        with tempfile.TemporaryDirectory() as tmpdir:
            target = Path(tmpdir) / "sub" / "dir" / "test.md"
            atomic_write(target, "nested content")
            self.assertTrue(target.exists())
            self.assertEqual(target.read_text(encoding="utf-8"), "nested content")


class TestRenderDataSchema(unittest.TestCase):

    def setUp(self):
        """Mock data matching real TCP command response shapes."""
        self.catalog = {
            "datatables": [
                {
                    "name": "DT_TestItems",
                    "path": "/Game/Data/DT_TestItems.DT_TestItems",
                    "row_struct": "FTestItemRow",
                    "row_count": 5,
                    "is_composite": False,
                    "parent_tables": [],
                    "top_fields": ["ItemName", "Value", "Category"],
                },
            ],
            "tag_prefixes": [
                {"prefix": "Test.Category", "count": 10},
            ],
            "data_asset_classes": [
                {"class_name": "TestDataAsset", "count": 2, "example_path": "/Game/Data/DA_Test"},
            ],
            "string_tables": [
                {"name": "ST_TestUI", "path": "/Game/Loc/ST_TestUI.ST_TestUI", "entry_count": 15},
            ],
        }
        self.schemas = {
            "FTestItemRow": {
                "struct_name": "FTestItemRow",
                "parent": "FTableRowBase",
                "schema": [
                    {"name": "ItemName", "type": "FName", "cpp_type": "FName"},
                    {"name": "Value", "type": "int32", "cpp_type": "int32", "default_value": "0"},
                    {"name": "Category", "type": "FGameplayTag", "cpp_type": "FGameplayTag"},
                ],
            },
        }
        self.example_rows = {
            "DT_TestItems": [
                {"row_name": "Item_Sword", "row_data": {"ItemName": "Sword", "Value": 100, "Category": "Test.Category.Weapon"}},
                {"row_name": "Item_Shield", "row_data": {"ItemName": "Shield", "Value": 50, "Category": "Test.Category.Armor"}},
            ],
        }
        self.curve_tables = []
        self.enum_values = {}

    def test_render_contains_datatable_summary(self):
        result = render_data_schema(
            self.catalog, self.schemas, self.example_rows,
            self.curve_tables, self.enum_values,
        )
        self.assertIn("DT_TestItems", result)
        self.assertIn("FTestItemRow", result)
        self.assertIn("/Game/Data/DT_TestItems", result)

    def test_render_contains_struct_schema(self):
        result = render_data_schema(
            self.catalog, self.schemas, self.example_rows,
            self.curve_tables, self.enum_values,
        )
        self.assertIn("## Struct Schemas", result)
        self.assertIn("### FTestItemRow", result)
        self.assertIn("name: ItemName", result)
        self.assertIn("type: FName", result)

    def test_render_contains_example_rows(self):
        result = render_data_schema(
            self.catalog, self.schemas, self.example_rows,
            self.curve_tables, self.enum_values,
        )
        self.assertIn("## Example Rows", result)
        self.assertIn("Item_Sword", result)

    def test_render_contains_tag_prefixes(self):
        result = render_data_schema(
            self.catalog, self.schemas, self.example_rows,
            self.curve_tables, self.enum_values,
        )
        self.assertIn("Test.Category", result)
        self.assertIn("count: 10", result)

    def test_render_contains_meta_header(self):
        result = render_data_schema(
            self.catalog, self.schemas, self.example_rows,
            self.curve_tables, self.enum_values,
        )
        self.assertIn("schema-meta", result)
        self.assertIn(f"schema_version: {SCHEMA_VERSION}", result)
        self.assertIn("domain: data", result)

    def test_render_contains_data_assets(self):
        result = render_data_schema(
            self.catalog, self.schemas, self.example_rows,
            self.curve_tables, self.enum_values,
        )
        self.assertIn("TestDataAsset", result)
        self.assertIn("count: 2", result)

    def test_render_contains_string_tables(self):
        result = render_data_schema(
            self.catalog, self.schemas, self.example_rows,
            self.curve_tables, self.enum_values,
        )
        self.assertIn("ST_TestUI", result)


class TestRenderCatalog(unittest.TestCase):

    def setUp(self):
        self.data_summary = {
            "structs": [
                {"name": "FTestItemRow", "used_by": "DT_TestItems"},
            ],
            "tables": [
                {"name": "DT_TestItems", "row_struct": "FTestItemRow", "rows": 5},
            ],
            "tag_prefixes": [
                {"prefix": "Test.Category.*", "count": 10},
            ],
            "data_assets": [
                {"class": "TestDataAsset", "instances": 2},
            ],
        }

    def test_catalog_contains_overview_table(self):
        result = render_catalog(
            project_name="TestProject",
            data_summary=self.data_summary,
        )
        self.assertIn("## Schema Overview", result)
        self.assertIn("| data |", result)

    def test_catalog_contains_index(self):
        result = render_catalog(
            project_name="TestProject",
            data_summary=self.data_summary,
        )
        self.assertIn("## Schema Index", result)
        # v2: catalog shows struct name in summary line, not in table rows
        self.assertIn("FTestItemRow", result)
        # v2: table count shown as summary stat, not per-table rows
        self.assertIn("Tables:", result)

    def test_catalog_contains_how_to_use(self):
        result = render_catalog(
            project_name="TestProject",
            data_summary=self.data_summary,
        )
        self.assertIn("## How to Use", result)

    def test_catalog_index_has_struct_summary(self):
        result = render_catalog(
            project_name="TestProject",
            data_summary=self.data_summary,
        )
        # v2: structs listed as bold summary, not table rows
        self.assertIn("**Structs:** FTestItemRow", result)

    def test_catalog_index_has_table_count_summary(self):
        result = render_catalog(
            project_name="TestProject",
            data_summary=self.data_summary,
        )
        # v2: table count shown as summary stat
        self.assertIn("**Tables:** 1 total", result)


class TestRenderCatalogV2(unittest.TestCase):

    def setUp(self):
        self.data_summary = {
            "structs": [{"name": "FTestRow", "used_by": "DT_Test"}],
            "tables": [{"name": "DT_Test", "row_struct": "FTestRow", "rows": 5}],
            "tag_prefixes": [{"prefix": "Test.*", "count": 10}],
            "data_assets": [{"class": "TestAsset", "instances": 2}],
        }

    def test_catalog_references_data_subdirectory(self):
        result = render_catalog(project_name="Test", data_summary=self.data_summary)
        self.assertIn("data/_index.md", result)
        self.assertIn("data/structs.md", result)
        self.assertIn("data/formats.md", result)
        # Should NOT reference old data.md as a standalone entry
        # (data.md appears as substring in data/... paths, so check for exact reference)
        self.assertNotIn("| data.md |", result)

    def test_catalog_has_progressive_disclosure_guidance(self):
        result = render_catalog(project_name="Test", data_summary=self.data_summary)
        self.assertIn("table listing", result.lower())
        self.assertIn("struct", result.lower())
        self.assertIn("format", result.lower())


from unittest.mock import MagicMock


class TestCollectDataDomain(unittest.TestCase):

    def _make_connection(self):
        """Create a mock UEConnection with realistic responses."""
        conn = MagicMock()

        catalog_response = {
            "success": True,
            "data": {
                "datatables": [
                    {
                        "name": "DT_Test",
                        "path": "/Game/Data/DT_Test.DT_Test",
                        "row_struct": "FTestRow",
                        "row_count": 3,
                        "is_composite": False,
                        "parent_tables": [],
                        "top_fields": ["Name", "Value"],
                    },
                ],
                "tag_prefixes": [{"prefix": "Test", "count": 5}],
                "data_asset_classes": [
                    {"class_name": "TestAsset", "count": 1, "example_path": "/Game/DA_Test"},
                ],
                "string_tables": [],
            },
        }

        schema_response = {
            "success": True,
            "data": {
                "struct_name": "FTestRow",
                "schema": [
                    {"name": "Name", "type": "FName", "cpp_type": "FName"},
                    {"name": "Value", "type": "int32", "cpp_type": "int32"},
                ],
            },
        }

        query_response = {
            "success": True,
            "data": {
                "rows": [
                    {"row_name": "Row1", "row_data": {"Name": "Test", "Value": 42}},
                    {"row_name": "Row2", "row_data": {"Name": "Other", "Value": 99}},
                ],
                "total_count": 3,
            },
        }

        def mock_send(command, params=None, **kwargs):
            if command == "data.get_data_catalog":
                return catalog_response
            elif command == "data.get_datatable_schema":
                return schema_response
            elif command == "data.query_datatable":
                return query_response
            elif command == "data.list_curve_tables":
                return {"success": True, "data": {"curve_tables": []}}
            return {"success": True, "data": {}}

        conn.send_command.side_effect = mock_send
        conn.send_command_cached.side_effect = mock_send
        return conn

    def test_collect_returns_catalog(self):
        conn = self._make_connection()
        result = collect_data_domain(conn)
        self.assertIn("catalog", result)
        self.assertEqual(len(result["catalog"]["datatables"]), 1)

    def test_collect_returns_schemas(self):
        conn = self._make_connection()
        result = collect_data_domain(conn)
        self.assertIn("schemas", result)
        self.assertIn("FTestRow", result["schemas"])

    def test_collect_returns_example_rows(self):
        conn = self._make_connection()
        result = collect_data_domain(conn)
        self.assertIn("example_rows", result)
        self.assertIn("DT_Test", result["example_rows"])
        # Should have at most 2 example rows
        self.assertLessEqual(len(result["example_rows"]["DT_Test"]), 2)

    def test_collect_returns_summary(self):
        conn = self._make_connection()
        result = collect_data_domain(conn)
        self.assertIn("summary", result)
        self.assertEqual(len(result["summary"]["structs"]), 1)
        self.assertEqual(result["summary"]["structs"][0]["name"], "FTestRow")
        self.assertEqual(result["summary"]["structs"][0]["used_by"], "DT_Test")

    def test_excludes_file_filters_datatable(self):
        import tempfile
        conn = MagicMock()

        catalog_response = {
            "success": True,
            "data": {
                "datatables": [
                    {
                        "name": "DT_Items",
                        "path": "/Game/Data/DT_Items.DT_Items",
                        "row_struct": "FItemRow",
                        "row_count": 2,
                        "is_composite": False,
                        "parent_tables": [],
                        "top_fields": ["Name"],
                    },
                    {
                        "name": "DT_StarterTable",
                        "path": "/Game/StarterContent/DT_StarterTable.DT_StarterTable",
                        "row_struct": "FStarterRow",
                        "row_count": 1,
                        "is_composite": False,
                        "parent_tables": [],
                        "top_fields": ["Name"],
                    },
                ],
                "tag_prefixes": [],
                "data_asset_classes": [],
                "string_tables": [],
            },
        }

        schema_response = {
            "success": True,
            "data": {
                "struct_name": "FItemRow",
                "schema": [{"name": "Name", "type": "FName", "cpp_type": "FName"}],
            },
        }

        def mock_send(command, params=None, **kwargs):
            if command == "data.get_data_catalog":
                return catalog_response
            elif command == "data.get_datatable_schema":
                return schema_response
            elif command == "data.query_datatable":
                return {"success": True, "data": {"rows": [], "total_count": 0}}
            elif command == "data.list_curve_tables":
                return {"success": True, "data": {"curve_tables": []}}
            return {"success": True, "data": {}}

        conn.send_command.side_effect = mock_send
        conn.send_command_cached.side_effect = mock_send

        with tempfile.TemporaryDirectory() as tmpdir:
            excludes_dir = Path(tmpdir) / ".cortex" / "config"
            excludes_dir.mkdir(parents=True)
            (excludes_dir / "schema_excludes.txt").write_text(
                "StarterContent\n", encoding="utf-8"
            )

            result = collect_data_domain(conn, project_root=Path(tmpdir))

        tables = result["catalog"]["datatables"]
        table_names = [t["name"] for t in tables]
        self.assertIn("DT_Items", table_names)
        self.assertNotIn("DT_StarterTable", table_names)

    def test_collect_extracts_enum_values(self):
        conn = MagicMock()
        catalog_resp = {
            "success": True,
            "data": {
                "datatables": [{"name": "DT_E", "path": "/Game/DT_E.DT_E", "row_struct": "FERow",
                                 "row_count": 1, "is_composite": False, "parent_tables": [], "top_fields": []}],
                "tag_prefixes": [], "data_asset_classes": [], "string_tables": [],
            },
        }
        schema_resp = {
            "success": True,
            "data": {
                "struct_name": "FERow",
                "schema": [
                    {"name": "Quality", "type": "EItemQuality", "cpp_type": "EItemQuality",
                     "enum_values": ["Common", "Rare", "Epic"]},
                ],
            },
        }
        def mock_send(command, params=None, **kwargs):
            if command == "data.get_data_catalog":
                return catalog_resp
            elif command == "data.get_datatable_schema":
                return schema_resp
            elif command == "data.query_datatable":
                return {"success": True, "data": {"rows": [], "total_count": 0}}
            elif command == "data.list_curve_tables":
                return {"success": True, "data": {"curve_tables": []}}
            return {"success": True, "data": {}}
        conn.send_command.side_effect = mock_send
        conn.send_command_cached.side_effect = mock_send

        result = collect_data_domain(conn)
        self.assertIn("EItemQuality", result["enum_values"])
        self.assertEqual(result["enum_values"]["EItemQuality"], ["Common", "Rare", "Epic"])


class TestCollectDataDomainJsonStringResponse(unittest.TestCase):
    """Reproduce bug: TCP protocol returns data as JSON string, not dict."""

    def test_collect_handles_json_string_data_field(self):
        """collect_data_domain must decode JSON-string 'data' fields."""
        conn = MagicMock()

        catalog_data = {
            "datatables": [
                {
                    "name": "DT_Test",
                    "path": "/Game/Data/DT_Test.DT_Test",
                    "row_struct": "FTestRow",
                    "row_count": 3,
                    "is_composite": False,
                    "parent_tables": [],
                    "top_fields": ["Name", "Value"],
                },
            ],
            "tag_prefixes": [{"prefix": "Test", "count": 5}],
            "data_asset_classes": [
                {"class_name": "TestAsset", "count": 1, "example_path": "/Game/DA_Test"},
            ],
            "string_tables": [],
        }
        schema_data = {
            "struct_name": "FTestRow",
            "schema": [
                {"name": "Name", "type": "FName", "cpp_type": "FName"},
                {"name": "Value", "type": "int32", "cpp_type": "int32"},
            ],
        }
        query_data = {
            "rows": [
                {"row_name": "Row1", "row_data": {"Name": "Test", "Value": 42}},
            ],
            "total_count": 3,
        }
        curve_data = {"curve_tables": []}

        def mock_send(command, params=None, **kwargs):
            if command == "data.get_data_catalog":
                return {"success": True, "data": json.dumps(catalog_data)}
            elif command == "data.get_datatable_schema":
                return {"success": True, "data": json.dumps(schema_data)}
            elif command == "data.query_datatable":
                return {"success": True, "data": json.dumps(query_data)}
            elif command == "data.list_curve_tables":
                return {"success": True, "data": json.dumps(curve_data)}
            return {"success": True, "data": "{}"}

        conn.send_command.side_effect = mock_send

        result = collect_data_domain(conn)

        self.assertIn("catalog", result)
        self.assertEqual(len(result["catalog"]["datatables"]), 1)
        self.assertIn("FTestRow", result["schemas"])
        self.assertIn("DT_Test", result["example_rows"])
        self.assertEqual(len(result["summary"]["structs"]), 1)


class TestCollectDataDomainDictSchema(unittest.TestCase):
    """Real TCP format: schema is a dict with struct_name and fields."""

    def test_collect_handles_dict_schema_format(self):
        """Schema response may nest fields inside a dict, not a flat list."""
        conn = MagicMock()

        catalog_data = {
            "datatables": [
                {
                    "name": "DT_Test",
                    "path": "/Game/Data/DT_Test.DT_Test",
                    "row_struct": "FTestRow",
                    "row_count": 3,
                    "is_composite": False,
                    "parent_tables": [],
                    "top_fields": ["Name", "Value"],
                },
            ],
            "tag_prefixes": [],
            "data_asset_classes": [],
            "string_tables": [],
        }
        # Real TCP format: schema is a dict, not a list
        schema_data = {
            "table_path": "/Game/Data/DT_Test.DT_Test",
            "row_struct_name": "FTestRow",
            "schema": {
                "struct_name": "FTestRow",
                "fields": [
                    {"name": "Name", "type": "FName", "cpp_type": "FName"},
                    {"name": "Value", "type": "int32", "cpp_type": "int32"},
                ],
            },
        }
        query_data = {
            "rows": [
                {"row_name": "Row1", "row_data": {"Name": "Test", "Value": 42}},
            ],
            "total_count": 3,
        }

        def mock_send(command, params=None, **kwargs):
            if command == "data.get_data_catalog":
                return {"success": True, "data": catalog_data}
            elif command == "data.get_datatable_schema":
                return {"success": True, "data": schema_data}
            elif command == "data.query_datatable":
                return {"success": True, "data": query_data}
            elif command == "data.list_curve_tables":
                return {"success": True, "data": {"curve_tables": []}}
            return {"success": True, "data": {}}

        conn.send_command.side_effect = mock_send

        result = collect_data_domain(conn)

        self.assertIn("FTestRow", result["schemas"])
        schema = result["schemas"]["FTestRow"]
        # schema["schema"] should be a list of fields, not the raw dict
        self.assertIsInstance(schema["schema"], list)
        self.assertEqual(len(schema["schema"]), 2)
        self.assertEqual(schema["schema"][0]["name"], "Name")


class TestGenerateSchema(unittest.TestCase):

    def test_generate_data_writes_files(self):
        """Full integration: mock connection -> generate files -> verify content."""
        conn = MagicMock()

        catalog_resp = {
            "success": True,
            "data": {
                "datatables": [
                    {
                        "name": "DT_Test",
                        "path": "/Game/Data/DT_Test.DT_Test",
                        "row_struct": "FTestRow",
                        "row_count": 2,
                        "is_composite": False,
                        "parent_tables": [],
                        "top_fields": ["Name"],
                    },
                ],
                "tag_prefixes": [],
                "data_asset_classes": [],
                "string_tables": [],
            },
        }
        schema_resp = {
            "success": True,
            "data": {
                "struct_name": "FTestRow",
                "schema": [{"name": "Name", "type": "FName", "cpp_type": "FName"}],
            },
        }
        query_resp = {
            "success": True,
            "data": {"rows": [{"row_name": "R1", "row_data": {"Name": "X"}}], "total_count": 2},
        }
        curve_resp = {"success": True, "data": {"curve_tables": []}}

        def mock_send(command, params=None, **kwargs):
            if command == "data.get_data_catalog":
                return catalog_resp
            elif command == "data.get_datatable_schema":
                return schema_resp
            elif command == "data.query_datatable":
                return query_resp
            elif command == "data.list_curve_tables":
                return curve_resp
            return {"success": True, "data": {}}

        conn.send_command.side_effect = mock_send
        conn.send_command_cached.side_effect = mock_send

        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            result = generate_schema(conn, schema_dir, domain="data", project_name="Test")

            # Verify split files exist (v2 structure)
            self.assertTrue((schema_dir / "data" / "_index.md").exists())
            self.assertTrue((schema_dir / "data" / "structs.md").exists())
            self.assertTrue((schema_dir / "data" / "formats.md").exists())
            self.assertTrue((schema_dir / "_catalog.md").exists())
            # Old monolithic file should NOT be created
            self.assertFalse((schema_dir / "data.md").exists())

            # Verify data/_index.md content
            index_content = (schema_dir / "data" / "_index.md").read_text(encoding="utf-8")
            self.assertIn("DT_Test", index_content)
            self.assertIn("FTestRow", index_content)

            # Verify data/structs.md content
            structs_content = (schema_dir / "data" / "structs.md").read_text(encoding="utf-8")
            self.assertIn("FTestRow", structs_content)

            # Verify catalog content
            catalog_content = (schema_dir / "_catalog.md").read_text(encoding="utf-8")
            self.assertIn("## Schema Index", catalog_content)
            # v2: catalog shows struct summary and table count, not individual table names
            self.assertIn("FTestRow", catalog_content)
            self.assertIn("Tables:", catalog_content)

            # Verify return value
            self.assertIn("data_index", result["generated"])
            self.assertIn("data_structs", result["generated"])
            self.assertIn("data_formats", result["generated"])

    def test_generate_fetches_engine_version(self):
        """generate_schema calls get_status for engine/plugin version."""
        conn = MagicMock()

        status_resp = {
            "success": True,
            "data": {
                "engine_version": "5.6",
                "plugin_version": "1.2.0",
            },
        }
        catalog_resp = {
            "success": True,
            "data": {
                "datatables": [],
                "tag_prefixes": [],
                "data_asset_classes": [],
                "string_tables": [],
            },
        }

        def mock_send(command, params=None, **kwargs):
            if command == "get_status":
                return status_resp
            if command == "data.get_data_catalog":
                return catalog_resp
            if command == "data.list_curve_tables":
                return {"success": True, "data": {"curve_tables": []}}
            return {"success": True, "data": {}}

        conn.send_command.side_effect = mock_send

        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            generate_schema(conn, schema_dir, domain="data", project_name="Test")

            catalog_content = (schema_dir / "_catalog.md").read_text(encoding="utf-8")
            self.assertIn("engine: 5.6", catalog_content)
            self.assertIn("plugin: 1.2.0", catalog_content)

    def test_generate_handles_json_string_status(self):
        """get_status may also return JSON-string data field."""
        conn = MagicMock()

        status_data = {"engine_version": "5.6", "plugin_version": "1.3.0"}
        catalog_data = {
            "datatables": [],
            "tag_prefixes": [],
            "data_asset_classes": [],
            "string_tables": [],
        }

        def mock_send(command, params=None, **kwargs):
            if command == "get_status":
                return {"success": True, "data": json.dumps(status_data)}
            if command == "data.get_data_catalog":
                return {"success": True, "data": json.dumps(catalog_data)}
            if command == "data.list_curve_tables":
                return {"success": True, "data": json.dumps({"curve_tables": []})}
            return {"success": True, "data": "{}"}

        conn.send_command.side_effect = mock_send

        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            generate_schema(conn, schema_dir, domain="data", project_name="Test")

            catalog_content = (schema_dir / "_catalog.md").read_text(encoding="utf-8")
            self.assertIn("engine: 5.6", catalog_content)
            self.assertIn("plugin: 1.3.0", catalog_content)

    def test_generate_raises_when_data_domain_unavailable(self):
        """Connection errors during required domain generation should fail hard."""
        conn = MagicMock()

        def mock_send(command, params=None, **kwargs):
            if command == "get_status":
                return {"success": True, "data": {"engine_version": "5.6", "plugin_version": "1.2.0"}}
            if command == "data.get_data_catalog":
                raise ConnectionError("editor unavailable")
            return {"success": True, "data": {}}

        conn.send_command.side_effect = mock_send

        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            with self.assertRaises(ConnectionError):
                generate_schema(conn, schema_dir, domain="data", project_name="Test")

            self.assertFalse((schema_dir / "data.md").exists())
            self.assertFalse((schema_dir / "_catalog.md").exists())


class TestReadMetaFromFile(unittest.TestCase):

    def test_read_valid_meta(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "test.md"
            path.write_text(
                "# Test\n\n<!-- schema-meta\nschema_version: 1\n"
                "generated: 2026-02-24T14:30:00Z\ndomain: data\n-->\n\nContent.",
                encoding="utf-8",
            )
            meta = read_meta_from_file(path)
            self.assertIsNotNone(meta)
            self.assertEqual(meta["schema_version"], "1")
            self.assertEqual(meta["domain"], "data")
            self.assertEqual(meta["generated"], "2026-02-24T14:30:00Z")

    def test_read_missing_file(self):
        result = read_meta_from_file(Path("/nonexistent/path.md"))
        self.assertIsNone(result)

    def test_read_no_meta_block(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "no_meta.md"
            path.write_text("# Just a regular file\n\nNo meta here.", encoding="utf-8")
            result = read_meta_from_file(path)
            self.assertIsNone(result)

    def test_read_corrupt_meta_block(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "corrupt.md"
            # Meta block without closing -->
            path.write_text("<!-- schema-meta\nschema_version: 1\n", encoding="utf-8")
            result = read_meta_from_file(path)
            self.assertIsNone(result)

    def test_read_meta_with_extra_fields(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "extra.md"
            path.write_text(
                "<!-- schema-meta\nschema_version: 1\ngenerated: 2026-02-24T14:30:00Z\n"
                "domain: catalog\nproject: MyProject\nengine: 5.6\nplugin: 1.0.0\n-->\n",
                encoding="utf-8",
            )
            meta = read_meta_from_file(path)
            self.assertIsNotNone(meta)
            self.assertEqual(meta["project"], "MyProject")
            self.assertEqual(meta["engine"], "5.6")
            self.assertEqual(meta["plugin"], "1.0.0")


class TestRenderMetaExtra(unittest.TestCase):

    def test_render_meta_with_extra_fields(self):
        from cortex_mcp.schema_generator import _render_meta
        result = _render_meta("catalog", project="TestProject", engine="5.6")
        self.assertIn("domain: catalog", result)
        self.assertIn("project: TestProject", result)
        self.assertIn("engine: 5.6", result)

    def test_render_meta_without_extra(self):
        from cortex_mcp.schema_generator import _render_meta
        result = _render_meta("data")
        self.assertIn("domain: data", result)
        self.assertNotIn("project:", result)


class TestDecodeData(unittest.TestCase):
    """Unit tests for _decode_data helper."""

    def test_decodes_json_string(self):
        resp = {"data": '{"key": "value"}'}
        self.assertEqual(_decode_data(resp), {"key": "value"})

    def test_passes_through_dict(self):
        resp = {"data": {"key": "value"}}
        self.assertEqual(_decode_data(resp), {"key": "value"})

    def test_returns_fallback_on_invalid_json(self):
        resp = {"data": "not valid json"}
        self.assertEqual(_decode_data(resp), {})

    def test_returns_fallback_on_missing_data_key(self):
        self.assertEqual(_decode_data({}), {})

    def test_returns_fallback_on_none_data(self):
        self.assertEqual(_decode_data({"data": None}), {})

    def test_returns_fallback_on_empty_string(self):
        resp = {"data": ""}
        self.assertEqual(_decode_data(resp), {})

    def test_custom_fallback(self):
        resp = {"data": "bad"}
        self.assertEqual(_decode_data(resp, fallback=[]), [])

    def test_returns_fallback_when_json_decodes_to_non_dict(self):
        """json.loads can return a list or string — must still return dict."""
        resp = {"data": '["a", "b"]'}
        self.assertEqual(_decode_data(resp), {})

    def test_returns_fallback_when_json_decodes_to_bare_string(self):
        resp = {"data": '"hello"'}
        self.assertEqual(_decode_data(resp), {})

    def test_returns_fallback_when_raw_is_list(self):
        """data field could be a list instead of dict."""
        resp = {"data": ["a", "b"]}
        self.assertEqual(_decode_data(resp), {})

    def test_returns_fallback_when_raw_is_int(self):
        resp = {"data": 42}
        self.assertEqual(_decode_data(resp), {})


class TestFilterEngineTags(unittest.TestCase):

    def test_filters_engine_tag_prefixes(self):
        from cortex_mcp.schema_generator import filter_engine_tags
        tags = [
            {"prefix": "Game.Item", "count": 10},
            {"prefix": "EnhancedInput", "count": 50},
            {"prefix": "InputUserSettings", "count": 30},
            {"prefix": "Platform", "count": 20},
            {"prefix": "Input", "count": 15},
            {"prefix": "InputMode", "count": 5},
            {"prefix": "Player.Stats", "count": 8},
        ]
        result = filter_engine_tags(tags)
        prefixes = [t["prefix"] for t in result]
        self.assertIn("Game.Item", prefixes)
        self.assertIn("Player.Stats", prefixes)
        self.assertNotIn("EnhancedInput", prefixes)
        self.assertNotIn("InputUserSettings", prefixes)
        self.assertNotIn("Platform", prefixes)
        self.assertNotIn("Input", prefixes)
        self.assertNotIn("InputMode", prefixes)

    def test_preserves_all_project_tags(self):
        from cortex_mcp.schema_generator import filter_engine_tags
        tags = [
            {"prefix": "Ability", "count": 5},
            {"prefix": "Status", "count": 3},
        ]
        result = filter_engine_tags(tags)
        self.assertEqual(len(result), 2)


class TestCatalogVersionInfo(unittest.TestCase):

    def test_catalog_includes_engine_plugin_version(self):
        result = render_catalog(
            project_name="Test",
            engine_version="5.6",
            plugin_version="1.0.0",
        )
        self.assertIn("project: Test", result)
        self.assertIn("engine: 5.6", result)
        self.assertIn("plugin: 1.0.0", result)

    def test_catalog_omits_empty_versions(self):
        result = render_catalog(project_name="Test")
        self.assertIn("project: Test", result)
        self.assertNotIn("engine:", result)
        self.assertNotIn("plugin:", result)


class TestLoadSchemaExcludes(unittest.TestCase):

    def test_loads_excludes_from_file(self):
        from cortex_mcp.schema_generator import load_schema_excludes
        with tempfile.TemporaryDirectory() as tmpdir:
            excludes_path = Path(tmpdir) / "schema_excludes.txt"
            excludes_path.write_text(
                "# Marketplace assets\nSci-fi_UI_Pack\n\n# Epic samples\nStarterContent\n",
                encoding="utf-8",
            )
            result = load_schema_excludes(excludes_path)
            self.assertEqual(result, ["Sci-fi_UI_Pack", "StarterContent"])

    def test_returns_empty_if_file_missing(self):
        from cortex_mcp.schema_generator import load_schema_excludes
        result = load_schema_excludes(Path("/nonexistent/schema_excludes.txt"))
        self.assertEqual(result, [])

    def test_ignores_blank_lines_and_comments(self):
        from cortex_mcp.schema_generator import load_schema_excludes
        with tempfile.TemporaryDirectory() as tmpdir:
            excludes_path = Path(tmpdir) / "schema_excludes.txt"
            excludes_path.write_text("# comment\n\n  \nValidPattern\n  # indented comment\n", encoding="utf-8")
            result = load_schema_excludes(excludes_path)
            self.assertEqual(result, ["ValidPattern"])


class TestFilterExcludedPaths(unittest.TestCase):

    def test_filters_tables_matching_excludes(self):
        from cortex_mcp.schema_generator import filter_excluded_paths
        tables = [
            {"name": "DT_Items", "path": "/Game/Data/DT_Items.DT_Items"},
            {"name": "DT_UI", "path": "/Game/Sci-fi_UI_Pack/DT_UI.DT_UI"},
            {"name": "DT_Starter", "path": "/Game/StarterContent/DT_Starter.DT_Starter"},
        ]
        result = filter_excluded_paths(tables, ["Sci-fi_UI_Pack", "StarterContent"])
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["name"], "DT_Items")

    def test_no_excludes_returns_all(self):
        from cortex_mcp.schema_generator import filter_excluded_paths
        tables = [{"name": "DT_A", "path": "/Game/Data/DT_A.DT_A"}]
        result = filter_excluded_paths(tables, [])
        self.assertEqual(len(result), 1)

    def test_filters_by_custom_path_key(self):
        from cortex_mcp.schema_generator import filter_excluded_paths
        classes = [
            {"class_name": "DA_A", "example_path": "/Game/Data/DA_A"},
            {"class_name": "DA_Pack", "example_path": "/Game/Sci-fi_UI_Pack/DA_Pack"},
        ]
        result = filter_excluded_paths(classes, ["Sci-fi_UI_Pack"], path_key="example_path")
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["class_name"], "DA_A")


class TestRenderDataIndex(unittest.TestCase):

    def setUp(self):
        self.catalog = {
            "datatables": [
                {"name": "PT_Meds", "path": "/Game/Data/PT_Meds.PT_Meds", "row_struct": "RipProductDefinition", "row_count": 204, "is_composite": False, "parent_tables": []},
                {"name": "PT_Organics", "path": "/Game/Data/PT_Organics.PT_Organics", "row_struct": "RipProductDefinition", "row_count": 109, "is_composite": False, "parent_tables": []},
                {"name": "DT_SeamData", "path": "/Game/Data/DT_SeamData.DT_SeamData", "row_struct": "RipCutPointDataRow", "row_count": 3, "is_composite": False, "parent_tables": []},
                {"name": "CPT_Products", "path": "/Game/Data/CPT_Products.CPT_Products", "row_struct": "RipProductDefinition", "row_count": 671, "is_composite": True, "parent_tables": ["/Game/Data/PT_Meds", "/Game/Data/PT_Organics"]},
            ],
            "tag_prefixes": [{"prefix": "Med", "count": 10}],
            "data_asset_classes": [{"class_name": "RipRecipeAsset", "count": 5, "example_path": "/Game/Data/DA_Test"}],
            "string_tables": [],
        }

    def test_groups_tables_by_struct(self):
        from cortex_mcp.schema_generator import render_data_index
        result = render_data_index(self.catalog)
        self.assertIn("RipProductDefinition", result)
        self.assertIn("PT_Meds", result)
        self.assertIn("PT_Organics", result)

    def test_composite_tables_separate_section(self):
        from cortex_mcp.schema_generator import render_data_index
        result = render_data_index(self.catalog)
        self.assertIn("Composite", result)
        self.assertIn("CPT_Products", result)
        # Verify the composite source tables are shown with the arrow format
        self.assertIn("<-", result)

    def test_composites_not_in_regular_groups(self):
        from cortex_mcp.schema_generator import render_data_index
        result = render_data_index(self.catalog)
        # CPT_Products should not be in the regular struct group count
        # The struct group should list 2 tables (PT_Meds, PT_Organics), not 3
        self.assertIn("2 tables", result)

    def test_includes_tag_prefixes(self):
        from cortex_mcp.schema_generator import render_data_index
        result = render_data_index(self.catalog)
        self.assertIn("Med", result)

    def test_includes_data_assets(self):
        from cortex_mcp.schema_generator import render_data_index
        result = render_data_index(self.catalog)
        self.assertIn("RipRecipeAsset", result)

    def test_includes_meta_block(self):
        from cortex_mcp.schema_generator import render_data_index
        result = render_data_index(self.catalog)
        self.assertIn("schema-meta", result)
        self.assertIn("domain: data-index", result)


class TestTruncateNestedFields(unittest.TestCase):

    def test_collapses_engine_struct_at_depth_1(self):
        from cortex_mcp.schema_generator import truncate_nested_fields
        fields = [
            {
                "name": "BrushImage",
                "type": "SlateBrush",
                "fields": [
                    {
                        "name": "TintColor",
                        "type": "SlateColor",
                        "fields": [
                            {"name": "SpecifiedColor", "type": "LinearColor", "fields": [
                                {"name": "R", "type": "float"},
                                {"name": "G", "type": "float"},
                            ]},
                        ],
                    },
                    {"name": "ImageSize", "type": "Vector2D"},
                ],
            },
        ]
        result = truncate_nested_fields(fields, max_depth=3, engine_max_depth=1)
        # Engine struct SlateBrush should have no nested fields at depth > 1
        brush = result[0]
        self.assertEqual(brush["type"], "SlateBrush")
        self.assertNotIn("fields", brush)

    def test_preserves_project_struct_nesting(self):
        from cortex_mcp.schema_generator import truncate_nested_fields
        fields = [
            {
                "name": "ItemData",
                "type": "RipProductDefinition",
                "fields": [
                    {"name": "Name", "type": "FName"},
                    {"name": "Price", "type": "int32"},
                ],
            },
        ]
        result = truncate_nested_fields(fields, max_depth=3, engine_max_depth=1)
        self.assertIn("fields", result[0])
        self.assertEqual(len(result[0]["fields"]), 2)

    def test_caps_project_struct_at_max_depth(self):
        from cortex_mcp.schema_generator import truncate_nested_fields
        fields = [
            {
                "name": "Deep",
                "type": "ProjectDeep",
                "fields": [
                    {"name": "L1", "type": "ProjectL1", "fields": [
                        {"name": "L2", "type": "ProjectL2", "fields": [
                            {"name": "L3", "type": "ProjectL3", "fields": [
                                {"name": "L4", "type": "int32"},
                            ]},
                        ]},
                    ]},
                ],
            },
        ]
        result = truncate_nested_fields(fields, max_depth=3, engine_max_depth=1)
        # With max_depth=3, the condition is _current_depth < 2, so L2 (depth 2) has children cut
        l1 = result[0]["fields"][0]
        l2 = l1["fields"][0]
        self.assertNotIn("fields", l2)  # L2's children (L3) cut at depth 3


class TestRenderDataStructs(unittest.TestCase):

    def setUp(self):
        self.schemas = {
            "FTestRow": {
                "struct_name": "FTestRow",
                "parent": "FTableRowBase",
                "schema": [
                    {"name": "ItemName", "type": "FName"},
                    {"name": "Price", "type": "int32", "default_value": "0"},
                    {"name": "Tag", "type": "FGameplayTag"},
                    {"name": "Brush", "type": "SlateBrush", "fields": [
                        {"name": "TintColor", "type": "SlateColor", "fields": [
                            {"name": "R", "type": "float"},
                        ]},
                    ]},
                ],
            },
        }

    def test_renders_struct_with_fields(self):
        from cortex_mcp.schema_generator import render_data_structs
        result = render_data_structs(self.schemas)
        self.assertIn("FTestRow", result)
        self.assertIn("ItemName: FName", result)
        self.assertIn("Price: int32", result)

    def test_engine_struct_collapsed(self):
        from cortex_mcp.schema_generator import render_data_structs
        result = render_data_structs(self.schemas)
        self.assertIn("Brush: SlateBrush", result)
        # Should NOT contain nested TintColor fields
        self.assertNotIn("TintColor", result)

    def test_includes_default_values(self):
        from cortex_mcp.schema_generator import render_data_structs
        result = render_data_structs(self.schemas)
        self.assertIn("default: 0", result)

    def test_includes_meta_block(self):
        from cortex_mcp.schema_generator import render_data_structs
        result = render_data_structs(self.schemas)
        self.assertIn("schema-meta", result)
        self.assertIn("domain: data-structs", result)

    def test_includes_enum_values(self):
        from cortex_mcp.schema_generator import render_data_structs
        schemas = {
            "FEnumRow": {
                "struct_name": "FEnumRow",
                "parent": "FTableRowBase",
                "schema": [
                    {"name": "Quality", "type": "EQuality", "enum_values": ["Common", "Rare"]},
                ],
            },
        }
        result = render_data_structs(schemas)
        self.assertIn("Common", result)
        self.assertIn("Rare", result)


class TestCollectFormatExamples(unittest.TestCase):

    def test_one_example_per_struct(self):
        from cortex_mcp.schema_generator import collect_format_examples
        catalog = {
            "datatables": [
                {"name": "PT_Meds", "path": "/Game/PT_Meds.PT_Meds", "row_struct": "ProductDef", "row_count": 200, "is_composite": False},
                {"name": "PT_Organics", "path": "/Game/PT_Organics.PT_Organics", "row_struct": "ProductDef", "row_count": 100, "is_composite": False},
                {"name": "DT_Seam", "path": "/Game/DT_Seam.DT_Seam", "row_struct": "SeamRow", "row_count": 3, "is_composite": False},
            ],
        }
        example_rows = {
            "PT_Meds": [{"row_name": "Med1", "row_data": {"Name": "Aspirin", "Price": 10}}],
            "PT_Organics": [{"row_name": "Org1", "row_data": {"Name": "Biomass", "Price": 5}}],
            "DT_Seam": [{"row_name": "S1", "row_data": {"Data": [1, 2]}}],
        }
        result = collect_format_examples(catalog, example_rows)
        # Should have 2 entries (one per unique struct), not 3
        self.assertEqual(len(result), 2)
        self.assertIn("ProductDef", result)
        self.assertIn("SeamRow", result)

    def test_skips_composite_tables(self):
        from cortex_mcp.schema_generator import collect_format_examples
        catalog = {
            "datatables": [
                {"name": "PT_Meds", "path": "/Game/PT_Meds.PT_Meds", "row_struct": "ProductDef", "row_count": 200, "is_composite": False},
                {"name": "CPT_All", "path": "/Game/CPT_All.CPT_All", "row_struct": "ProductDef", "row_count": 671, "is_composite": True},
            ],
        }
        example_rows = {
            "PT_Meds": [{"row_name": "Med1", "row_data": {"Name": "Test"}}],
            "CPT_All": [{"row_name": "All1", "row_data": {"Name": "FromComposite"}}],
        }
        result = collect_format_examples(catalog, example_rows)
        self.assertEqual(len(result), 1)
        # Source should be the non-composite table
        self.assertEqual(result["ProductDef"]["source_table"], "PT_Meds")


class TestRenderDataFormats(unittest.TestCase):

    def test_renders_compact_table_format(self):
        from cortex_mcp.schema_generator import render_data_formats
        format_examples = {
            "ProductDef": {
                "source_table": "PT_Meds",
                "row_data": {"Name": "Aspirin", "Price": 10, "Tag": "Med.Painkiller"},
                "schemas": {"Name": "FName", "Price": "int32", "Tag": "FGameplayTag"},
            },
        }
        result = render_data_formats(format_examples)
        self.assertIn("ProductDef", result)
        self.assertIn("PT_Meds", result)
        self.assertIn("| Name |", result)
        self.assertIn("Aspirin", result)

    def test_truncates_long_arrays(self):
        from cortex_mcp.schema_generator import render_data_formats
        long_array = list(range(50))
        format_examples = {
            "ArrayRow": {
                "source_table": "DT_Test",
                "row_data": {"Items": long_array},
                "schemas": {"Items": "TArray<int32>"},
            },
        }
        result = render_data_formats(format_examples)
        self.assertIn("+47 more", result)

    def test_includes_meta_block(self):
        from cortex_mcp.schema_generator import render_data_formats
        result = render_data_formats({})
        self.assertIn("schema-meta", result)
        self.assertIn("domain: data-formats", result)


class TestGenerateSchemaV2(unittest.TestCase):

    def _make_connection(self):
        conn = MagicMock()

        catalog_data = {
            "datatables": [
                {"name": "DT_Test", "path": "/Game/Data/DT_Test.DT_Test", "row_struct": "FTestRow",
                 "row_count": 2, "is_composite": False, "parent_tables": [], "top_fields": ["Name"]},
            ],
            "tag_prefixes": [{"prefix": "Test", "count": 5}],
            "data_asset_classes": [],
            "string_tables": [],
        }
        schema_data = {
            "struct_name": "FTestRow",
            "schema": [{"name": "Name", "type": "FName", "cpp_type": "FName"}],
        }
        query_data = {
            "rows": [{"row_name": "R1", "row_data": {"Name": "TestVal"}}],
            "total_count": 2,
        }
        status_data = {"engine_version": "5.6", "plugin_version": "1.0.0"}

        def mock_send(command, params=None, **kwargs):
            if command == "get_status":
                return {"success": True, "data": status_data}
            if command == "data.get_data_catalog":
                return {"success": True, "data": catalog_data}
            elif command == "data.get_datatable_schema":
                return {"success": True, "data": schema_data}
            elif command == "data.query_datatable":
                return {"success": True, "data": query_data}
            elif command == "data.list_curve_tables":
                return {"success": True, "data": {"curve_tables": []}}
            return {"success": True, "data": {}}

        conn.send_command.side_effect = mock_send
        return conn

    def test_generates_split_files(self):
        conn = self._make_connection()
        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            result = generate_schema(conn, schema_dir, domain="data", project_name="Test")

            self.assertTrue((schema_dir / "data" / "_index.md").exists())
            self.assertTrue((schema_dir / "data" / "structs.md").exists())
            self.assertTrue((schema_dir / "data" / "formats.md").exists())
            self.assertTrue((schema_dir / "_catalog.md").exists())
            # Old monolithic file should NOT be created
            self.assertFalse((schema_dir / "data.md").exists())

    def test_deletes_old_data_md(self):
        conn = self._make_connection()
        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            schema_dir.mkdir(parents=True)
            # Create old v1 file
            old_file = schema_dir / "data.md"
            old_file.write_text("old v1 content", encoding="utf-8")

            generate_schema(conn, schema_dir, domain="data", project_name="Test")

            self.assertFalse(old_file.exists())

    def test_schema_version_is_2(self):
        from cortex_mcp.schema_generator import SCHEMA_VERSION
        self.assertEqual(SCHEMA_VERSION, 2)

    def test_catalog_references_subdirectory(self):
        conn = self._make_connection()
        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            generate_schema(conn, schema_dir, domain="data", project_name="Test")

            catalog = (schema_dir / "_catalog.md").read_text(encoding="utf-8")
            self.assertIn("data/_index.md", catalog)
            self.assertIn("data/structs.md", catalog)
            self.assertIn("data/formats.md", catalog)

    def test_result_contains_all_files(self):
        conn = self._make_connection()
        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            result = generate_schema(conn, schema_dir, domain="data", project_name="Test")

            self.assertIn("data_index", result["generated"])
            self.assertIn("data_structs", result["generated"])
            self.assertIn("data_formats", result["generated"])


class TestSchemaStatusV2(unittest.TestCase):

    def test_status_detects_v2_subdirectory(self):
        """schema_status should detect data/ subdirectory structure."""
        # This is an integration test — we test the read_meta_from_file path
        # since schema_status depends on the MCP server wrapper
        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            data_dir = schema_dir / "data"
            data_dir.mkdir(parents=True)

            # Write v2 files with meta blocks
            from cortex_mcp.schema_generator import _render_meta
            (data_dir / "_index.md").write_text(
                f"# Index\n\n{_render_meta('data-index')}\n",
                encoding="utf-8",
            )
            (data_dir / "structs.md").write_text(
                f"# Structs\n\n{_render_meta('data-structs')}\n",
                encoding="utf-8",
            )

            # Verify meta can be read from subdirectory files
            from cortex_mcp.schema_generator import read_meta_from_file
            meta = read_meta_from_file(data_dir / "_index.md")
            self.assertIsNotNone(meta)
            self.assertEqual(meta["domain"], "data-index")
            self.assertEqual(meta["schema_version"], "2")
