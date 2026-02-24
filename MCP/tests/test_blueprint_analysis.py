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


def test_analyze_blueprint_for_migration_wires_command():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {
        "data": {
            "name": "BP_Test",
            "asset_path": "/Game/Blueprints/BP_Test",
            "complexity_metrics": {"migration_confidence": "high"},
        }
    }

    register_blueprint_analysis_tools(mcp, connection)
    assert "analyze_blueprint_for_migration" in mcp.tools

    result = mcp.tools["analyze_blueprint_for_migration"]("/Game/Blueprints/BP_Test")
    parsed = json.loads(result)
    assert parsed["name"] == "BP_Test"
    assert parsed["complexity_metrics"]["migration_confidence"] == "high"
    connection.send_command.assert_called_once_with(
        "bp.analyze_for_migration",
        {"asset_path": "/Game/Blueprints/BP_Test"},
    )
