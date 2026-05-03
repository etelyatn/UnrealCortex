"""Unit tests for graph MCP tools."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock

TOOLS_DIR = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from graph import register_graph_domain_tools


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
    register_graph_domain_tools(mcp, connection)
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
            "compact": True,
            "function_name": "PrintString",
        },
        ttl=120,
    )


def test_graph_trace_exec_sends_cached_trace_command():
    connection = MagicMock()
    connection.send_command_cached.return_value = {
        "data": {
            "nodes": [{"node_id": "K2Node_Event_0"}, {"node_id": "K2Node_CallFunction_0"}],
            "edges": [{"source_node": "K2Node_Event_0", "target_node": "K2Node_CallFunction_0"}],
        }
    }

    tools = _register_tools(connection)
    result = tools["graph_trace_exec"](
        asset_path="/Game/BP_Test",
        start_node_id="K2Node_Event_0",
        max_depth=12,
        traverse_policy="Opaque",
        include_edges=True,
    )
    parsed = json.loads(result)

    assert len(parsed["nodes"]) == 2
    connection.send_command_cached.assert_called_once_with(
        "graph.trace_exec",
        {
            "asset_path": "/Game/BP_Test",
            "start_node_id": "K2Node_Event_0",
            "max_depth": 12,
            "traverse_policy": "Opaque",
            "include_edges": True,
        },
        ttl=120,
    )


def test_repo_has_no_active_legacy_graph_read_refs():
    repo_root = Path(__file__).resolve().parents[3]
    targets = [
        repo_root / "Plugins/UnrealCortex/MCP",
        repo_root / "cortex-toolkit/agents",
        repo_root / "cortex-toolkit/resources",
        repo_root / "docs/systems",
    ]
    forbidden = ("graph_list_nodes", "graph_get_node", "graph.list_nodes", "graph.get_node")
    hits: list[str] = []

    for target in targets:
        for path in target.rglob("*"):
            if path.suffix not in {".py", ".md", ".json"}:
                continue
            text = path.read_text(encoding="utf-8")
            if any(token in text for token in forbidden):
                hits.append(str(path.relative_to(repo_root)))

    assert not hits, f"Found active legacy graph read refs: {hits}"
