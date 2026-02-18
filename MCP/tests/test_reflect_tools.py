"""Unit tests for reflect tool registration and response formatting."""

import sys
from pathlib import Path
from unittest.mock import MagicMock

import pytest

# Add tools/ to sys.path matching existing test patterns
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))


class TestReflectToolRegistration:
    """Test that reflect tools register correctly."""

    def test_hierarchy_registration(self):
        from reflect.hierarchy import register_reflect_hierarchy_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_hierarchy_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_detail_registration(self):
        from reflect.detail import register_reflect_detail_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_detail_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_overrides_registration(self):
        from reflect.overrides import register_reflect_override_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_override_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_usages_registration(self):
        from reflect.usages import register_reflect_usage_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_usage_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_cache_registration(self):
        from reflect.cache import register_reflect_cache_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_cache_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_context_registration(self):
        from reflect.context import register_reflect_context_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_context_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_init_registers_all(self):
        from reflect import register_reflect_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_tools(mcp, connection)
        assert mcp.tool.call_count >= 6


class TestHierarchyFlattening:
    """Test the tree-to-flat-list conversion."""

    def test_flatten_simple_tree(self):
        from reflect.hierarchy import _flatten_hierarchy

        tree = {
            "name": "AActor",
            "type": "cpp",
            "module": "Engine",
            "children": [
                {
                    "name": "AMyActor",
                    "type": "cpp",
                    "module": "MyGame",
                    "source_path": "MyActor.h",
                    "children": [],
                }
            ],
            "total_classes": 2,
            "cpp_count": 2,
            "blueprint_count": 0,
        }

        result = _flatten_hierarchy(tree)
        assert "classes" in result
        assert len(result["classes"]) == 2
        assert result["classes"][0]["name"] == "AActor"
        assert result["classes"][0]["depth"] == 0
        assert result["classes"][0]["parent"] is None
        assert result["classes"][1]["name"] == "AMyActor"
        assert result["classes"][1]["depth"] == 1
        assert result["classes"][1]["parent"] == "AActor"

    def test_flatten_preserves_counts(self):
        from reflect.hierarchy import _flatten_hierarchy

        tree = {
            "name": "AActor",
            "type": "cpp",
            "children": [],
            "total_classes": 1,
            "cpp_count": 1,
            "blueprint_count": 0,
        }

        result = _flatten_hierarchy(tree)
        assert result["total_classes"] == 1
        assert result["cpp_count"] == 1
        assert result["blueprint_count"] == 0

    def test_flatten_empty_name_defensive(self):
        from reflect.hierarchy import _flatten_hierarchy

        tree = {"children": [], "total_classes": 1, "cpp_count": 1, "blueprint_count": 0}
        result = _flatten_hierarchy(tree)
        assert result["classes"][0]["name"] == "Unknown"


class TestDetailLevels:
    """Test detail level pruning."""

    def test_summary_level(self):
        from reflect.detail import _prune_detail

        full_data = {
            "name": "AMyActor",
            "type": "cpp",
            "parent": "AActor",
            "module": "MyGame",
            "source_path": "MyActor.h",
            "properties": [{"name": "Health", "type": "float"}],
            "functions": [{"name": "TakeDamage", "return_type": "float"}],
            "components": [],
            "interfaces": [],
            "blueprint_children_count": 2,
        }

        result = _prune_detail(full_data, "summary")
        assert "name" in result
        assert "type" in result
        assert "parent" in result
        assert "source_path" in result  # included at summary level
        assert "properties" not in result
        assert "functions" not in result

    def test_properties_level(self):
        from reflect.detail import _prune_detail

        full_data = {
            "name": "AMyActor",
            "type": "cpp",
            "parent": "AActor",
            "module": "MyGame",
            "properties": [{"name": "Health", "type": "float"}],
            "functions": [{"name": "TakeDamage", "return_type": "float"}],
            "components": [{"name": "Mesh", "type": "UStaticMeshComponent"}],
            "interfaces": [],
            "blueprint_children_count": 2,
        }

        result = _prune_detail(full_data, "properties")
        assert "properties" in result
        assert "components" in result  # included at properties level
        assert "interfaces" in result
        assert "functions" not in result

    def test_full_level(self):
        from reflect.detail import _prune_detail

        full_data = {
            "name": "AMyActor",
            "type": "cpp",
            "parent": "AActor",
            "module": "MyGame",
            "source_path": "MyActor.h",
            "properties": [{"name": "Health", "type": "float"}],
            "functions": [{"name": "TakeDamage", "return_type": "float"}],
            "components": [],
            "interfaces": [],
            "blueprint_children_count": 2,
        }

        result = _prune_detail(full_data, "full")
        assert "properties" in result
        assert "functions" in result
        assert "components" in result
