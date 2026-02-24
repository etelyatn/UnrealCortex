"""Unit tests for schema_generator module."""

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from cortex_mcp.schema_generator import find_project_root, get_schema_dir
from cortex_mcp.schema_generator import render_data_schema, SCHEMA_VERSION


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
