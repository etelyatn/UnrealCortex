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
        self.assertIn("FTestItemRow", result)
        self.assertIn("DT_TestItems", result)

    def test_catalog_contains_how_to_use(self):
        result = render_catalog(
            project_name="TestProject",
            data_summary=self.data_summary,
        )
        self.assertIn("## How to Use", result)

    def test_catalog_index_has_struct_used_by(self):
        result = render_catalog(
            project_name="TestProject",
            data_summary=self.data_summary,
        )
        # The struct index table should have "Used By" column
        self.assertIn("| FTestItemRow | DT_TestItems |", result)

    def test_catalog_index_has_table_row_struct(self):
        result = render_catalog(
            project_name="TestProject",
            data_summary=self.data_summary,
        )
        self.assertIn("| DT_TestItems | FTestItemRow |", result)


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

            # Verify files exist
            self.assertTrue((schema_dir / "data.md").exists())
            self.assertTrue((schema_dir / "_catalog.md").exists())

            # Verify data.md content
            data_content = (schema_dir / "data.md").read_text(encoding="utf-8")
            self.assertIn("DT_Test", data_content)
            self.assertIn("FTestRow", data_content)

            # Verify catalog content
            catalog_content = (schema_dir / "_catalog.md").read_text(encoding="utf-8")
            self.assertIn("## Schema Index", catalog_content)
            self.assertIn("DT_Test", catalog_content)

            # Verify return value
            self.assertIn("data", result["generated"])

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
