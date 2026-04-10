"""Unit tests for Blueprint migration MCP tool wrappers."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock

TOOLS_DIR = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from blueprint.migration import register_blueprint_migration_tools


class MockMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator


def _register_tools(connection):
    mcp = MockMCP()
    register_blueprint_migration_tools(mcp, connection)
    return mcp.tools


def test_remove_scs_component_forwards_acknowledgment_and_force():
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"removed_component": "OpenSeq"}}

    tools = _register_tools(connection)
    result = tools["remove_scs_component"](
        asset_path="/Game/BP_Test",
        component_name="OpenSeq",
        compile=False,
        acknowledged_losses=["Payload"],
        force=True,
    )

    parsed = json.loads(result)
    assert parsed["removed_component"] == "OpenSeq"
    connection.send_command.assert_called_once_with(
        "blueprint.remove_scs_component",
        {
            "asset_path": "/Game/BP_Test",
            "component_name": "OpenSeq",
            "compile": False,
            "acknowledged_losses": ["Payload"],
            "force": True,
        },
    )


def test_rename_scs_component_forwards_compile_flag():
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"new_name": "LegacyOpenSeq"}}

    tools = _register_tools(connection)
    result = tools["rename_scs_component"](
        asset_path="/Game/BP_Test",
        old_name="OpenSeq",
        new_name="LegacyOpenSeq",
        compile=False,
    )

    parsed = json.loads(result)
    assert parsed["new_name"] == "LegacyOpenSeq"
    connection.send_command.assert_called_once_with(
        "blueprint.rename_scs_component",
        {
            "asset_path": "/Game/BP_Test",
            "old_name": "OpenSeq",
            "new_name": "LegacyOpenSeq",
            "compile": False,
        },
    )
