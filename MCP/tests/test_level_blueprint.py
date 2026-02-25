"""Tests for level_blueprint MCP tool."""
import json
import sys
import os
import pytest
from unittest.mock import MagicMock

# Add MCP root to path so 'from tools.blueprint.* import' works
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
# Add src to path for cortex_mcp imports inside the tool
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))


def _register_and_get_tool(mock_conn=None):
    """Helper: register tools and return the get_level_blueprint function."""
    from tools.blueprint.level_blueprint import register_level_blueprint_tools

    tools_registered = []

    class MockMcp:
        def tool(self):
            def decorator(fn):
                tools_registered.append(fn)
                return fn
            return decorator

    mcp = MockMcp()
    register_level_blueprint_tools(mcp, mock_conn or MagicMock())
    assert len(tools_registered) == 1, f"Expected 1 tool, got {len(tools_registered)}"
    return tools_registered[0]


def test_get_level_blueprint_returns_synthetic_path():
    get_level_blueprint = _register_and_get_tool()
    result = json.loads(get_level_blueprint(map_path="/Game/Maps/TestMap"))
    assert result["asset_path"] == "__level_bp__:/Game/Maps/TestMap"
    assert result["map_path"] == "/Game/Maps/TestMap"
    assert result["is_level_blueprint"] is True
    assert "save_warning" in result


def test_get_level_blueprint_strips_trailing_slash():
    get_level_blueprint = _register_and_get_tool()
    result = json.loads(get_level_blueprint(map_path="/Game/Maps/TestMap/"))
    assert result["asset_path"] == "__level_bp__:/Game/Maps/TestMap"
    assert result["map_path"] == "/Game/Maps/TestMap"


def test_get_level_blueprint_adds_leading_slash():
    get_level_blueprint = _register_and_get_tool()
    result = json.loads(get_level_blueprint(map_path="Game/Maps/TestMap"))
    assert result["asset_path"] == "__level_bp__:/Game/Maps/TestMap"


def test_save_warning_mentions_save_level():
    get_level_blueprint = _register_and_get_tool()
    result = json.loads(get_level_blueprint(map_path="/Game/Maps/TestMap"))
    assert "save_level" in result["save_warning"]
    assert "/Game/Maps/TestMap" in result["save_warning"]
