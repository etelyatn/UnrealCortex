"""Unit tests for Blueprint migration analysis MCP tools."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock

# Add tools directory to path for imports
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from blueprint.analysis import register_blueprint_analysis_tools


class MockMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator


def _setup():
    """Create MCP and connection mocks, register tools, return (mcp, connection)."""
    mcp = MockMCP()
    connection = MagicMock()
    register_blueprint_analysis_tools(mcp, connection)
    return mcp, connection


def test_analyze_blueprint_for_migration_wires_command():
    mcp, connection = _setup()
    connection.send_command.return_value = {
        "data": {
            "name": "BP_Test",
            "asset_path": "/Game/Blueprints/BP_Test",
            "complexity_metrics": {"migration_confidence": "high"},
        }
    }

    assert "analyze_blueprint_for_migration" in mcp.tools

    result = mcp.tools["analyze_blueprint_for_migration"]("/Game/Blueprints/BP_Test")
    parsed = json.loads(result)
    assert parsed["name"] == "BP_Test"
    assert parsed["complexity_metrics"]["migration_confidence"] == "high"
    connection.send_command.assert_called_once_with(
        "blueprint.analyze_for_migration",
        {"asset_path": "/Game/Blueprints/BP_Test"},
    )


def test_connection_error_returns_json_error():
    mcp, connection = _setup()
    connection.send_command.side_effect = ConnectionError("TCP refused")

    result = mcp.tools["analyze_blueprint_for_migration"]("/Game/BP_Test")
    parsed = json.loads(result)
    assert "error" in parsed
    assert "Connection error" in parsed["error"]


def test_runtime_error_returns_json_error():
    mcp, connection = _setup()
    connection.send_command.side_effect = RuntimeError("command failed")

    result = mcp.tools["analyze_blueprint_for_migration"]("/Game/BP_Test")
    parsed = json.loads(result)
    assert "error" in parsed
    assert "command failed" in parsed["error"]


def test_timeout_error_returns_json_error():
    mcp, connection = _setup()
    connection.send_command.side_effect = TimeoutError("read timed out")

    result = mcp.tools["analyze_blueprint_for_migration"]("/Game/BP_Test")
    parsed = json.loads(result)
    assert "error" in parsed
    assert "read timed out" in parsed["error"]


def test_os_error_returns_json_error():
    mcp, connection = _setup()
    connection.send_command.side_effect = OSError("socket closed")

    result = mcp.tools["analyze_blueprint_for_migration"]("/Game/BP_Test")
    parsed = json.loads(result)
    assert "error" in parsed
    assert "socket closed" in parsed["error"]


def test_empty_data_returns_empty_fields():
    mcp, connection = _setup()
    connection.send_command.return_value = {"data": {}}

    result = mcp.tools["analyze_blueprint_for_migration"]("/Game/BP_Test")
    parsed = json.loads(result)
    # Should not error — just returns the empty data
    assert "error" not in parsed
