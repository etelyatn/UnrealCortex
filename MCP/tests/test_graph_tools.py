"""Unit tests for graph MCP tools."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock

TOOLS_DIR = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from graph.graph import register_graph_tools


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
    register_graph_tools(mcp, connection)
    return mcp.tools


def test_graph_search_nodes_sends_cached_search_command():
    connection = MagicMock()
    connection.send_command_cached.return_value = {
        "data": {
            "results": [{"node_id": "K2Node_CallFunction_0"}],
            "count": 1,
        }
    }

    tools = _register_tools(connection)
    result = tools["graph_search_nodes"](asset_path="/Game/BP_Test", function_name="PrintString")
    parsed = json.loads(result)

    assert parsed["count"] == 1
    connection.send_command_cached.assert_called_once_with(
        "graph.search_nodes",
        {
            "asset_path": "/Game/BP_Test",
            "function_name": "PrintString",
        },
        ttl=120,
    )
