"""Unit tests for schema_generator module."""

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from cortex_mcp.schema_generator import find_project_root, get_schema_dir


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
