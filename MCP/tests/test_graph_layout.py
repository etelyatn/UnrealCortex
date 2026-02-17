"""Unit tests for graph layout MCP tool."""

import sys
from pathlib import Path
from unittest.mock import MagicMock

# Add tools directory to path for imports
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))


def test_register_graph_layout_tools_importable():
    from graph.layout import register_graph_layout_tools
    assert callable(register_graph_layout_tools)


def test_graph_auto_layout_calls_correct_command():
    """Verify the tool sends graph.auto_layout over TCP with correct params."""
    from graph.layout import register_graph_layout_tools

    mcp = MagicMock()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"node_count": 5, "asset_path": "/Game/BP"}}

    register_graph_layout_tools(mcp, connection)

    # mcp.tool() was called to get the decorator, then decorator was called with the function
    tool_func = mcp.tool.return_value.call_args[0][0]
    result = tool_func(asset_path="/Game/BP_Test", mode="full")

    connection.send_command.assert_called_once_with(
        "graph.auto_layout", {"asset_path": "/Game/BP_Test", "mode": "full"}
    )
    connection.invalidate_cache.assert_any_call("graph.")
    connection.invalidate_cache.assert_any_call("bp.")


def test_graph_auto_layout_optional_params_excluded_when_none():
    """Optional params (graph_name, spacing) are not sent when not provided."""
    from graph.layout import register_graph_layout_tools

    mcp = MagicMock()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {}}

    register_graph_layout_tools(mcp, connection)
    tool_func = mcp.tool.return_value.call_args[0][0]
    tool_func(asset_path="/Game/BP_Test")

    call_args = connection.send_command.call_args
    params = call_args[0][1]
    assert "graph_name" not in params
    assert "horizontal_spacing" not in params
    assert "vertical_spacing" not in params


def test_graph_auto_layout_optional_params_included_when_provided():
    """Optional params are included in the command when provided."""
    from graph.layout import register_graph_layout_tools

    mcp = MagicMock()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {}}

    register_graph_layout_tools(mcp, connection)
    tool_func = mcp.tool.return_value.call_args[0][0]
    tool_func(
        asset_path="/Game/BP_Test",
        mode="incremental",
        graph_name="EventGraph",
        horizontal_spacing=100,
        vertical_spacing=60,
    )

    connection.send_command.assert_called_once_with(
        "graph.auto_layout",
        {
            "asset_path": "/Game/BP_Test",
            "mode": "incremental",
            "graph_name": "EventGraph",
            "horizontal_spacing": 100,
            "vertical_spacing": 60,
        },
    )


def test_graph_auto_layout_returns_json_string():
    """Tool returns a JSON-formatted string from the response data."""
    import json
    from graph.layout import register_graph_layout_tools

    mcp = MagicMock()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"node_count": 3, "asset_path": "/Game/BP"}}

    register_graph_layout_tools(mcp, connection)
    tool_func = mcp.tool.return_value.call_args[0][0]
    result = tool_func(asset_path="/Game/BP")

    parsed = json.loads(result)
    assert parsed["node_count"] == 3
    assert parsed["asset_path"] == "/Game/BP"


def test_graph_auto_layout_invalidates_both_caches():
    """Cache invalidation clears both graph and bp namespaces."""
    from graph.layout import register_graph_layout_tools

    mcp = MagicMock()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {}}

    register_graph_layout_tools(mcp, connection)
    tool_func = mcp.tool.return_value.call_args[0][0]
    tool_func(asset_path="/Game/BP_Test", mode="full")

    invalidate_calls = [c[0][0] for c in connection.invalidate_cache.call_args_list]
    assert "graph." in invalidate_calls
    assert "bp." in invalidate_calls
