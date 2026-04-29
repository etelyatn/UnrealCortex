"""Wiring tests for the statetree.* domain.

PR 1 verifies that:
  - `statetree` is registered in CORE_DOMAINS so `register_router_tools`
    auto-creates `statetree_cmd`.
  - `_FALLBACK_STRUCTURED` includes `statetree` so `capabilities.py` import
    does not raise.
  - The `statetree_cmd` router forwards commands as `statetree.<command>` to
    the underlying connection.

Subsequent PRs add per-feature wrapper-tool tests under
`test_statetree_assets.py`, `test_statetree_states.py`, etc.
"""

import json
from unittest.mock import MagicMock

from cortex_mcp.capabilities import CORE_DOMAINS
from cortex_mcp._fallback_generated import FALLBACK_COMMANDS as _FALLBACK_STRUCTURED
from cortex_mcp.tools.routers import register_router_tools


class MockMCP:
    def __init__(self):
        self.tools = {}

    def tool(self, name=None, description=None):
        def decorator(fn):
            tool_name = name or fn.__name__
            self.tools[tool_name] = fn
            return fn

        return decorator


def _setup_router(domain: str = "statetree"):
    mcp = MockMCP()
    connection = MagicMock()
    register_router_tools(mcp, connection, docstrings={domain: ""}, domains=(domain,))
    return mcp, connection


def test_statetree_in_core_domains():
    """statetree must be a CORE domain so `statetree_cmd` is auto-registered."""
    assert "statetree" in CORE_DOMAINS


def test_statetree_in_fallback_commands():
    """Fallback must include statetree so capabilities.py import does not raise.

    `cortex_mcp.capabilities` lines 67-72 enforce this with an ImportError.
    """
    assert "statetree" in _FALLBACK_STRUCTURED
    fallback = _FALLBACK_STRUCTURED["statetree"]
    assert isinstance(fallback, list)
    assert len(fallback) >= 1, "statetree fallback must have at least one command"
    names = {cmd["name"] for cmd in fallback}
    assert "get_status" in names


def test_statetree_cmd_tool_registered():
    """register_router_tools must create a `statetree_cmd` MCP tool."""
    mcp, _ = _setup_router()
    assert "statetree_cmd" in mcp.tools


def test_statetree_cmd_forwards_qualified_command():
    """statetree_cmd(command, params) must call connection.send_command('statetree.<cmd>', params)."""
    mcp, connection = _setup_router()
    connection.send_command.return_value = {
        "data": {"domain": "statetree", "version": "1.0.0", "registered": True, "command_count": 1}
    }

    result = mcp.tools["statetree_cmd"]("get_status", {})
    parsed = json.loads(result)

    assert parsed["domain"] == "statetree"
    assert parsed["registered"] is True
    connection.send_command.assert_called_once_with("statetree.get_status", {})


def test_statetree_cmd_passes_params_through():
    """Params dict should be forwarded verbatim to the C++ side."""
    mcp, connection = _setup_router()
    connection.send_command.return_value = {"data": {}}

    mcp.tools["statetree_cmd"]("some_future_command", {"asset_path": "/Game/AI/ST_Boss", "depth": 3})

    connection.send_command.assert_called_once_with(
        "statetree.some_future_command",
        {"asset_path": "/Game/AI/ST_Boss", "depth": 3},
    )


def test_statetree_cmd_handles_connection_error():
    """ConnectionError should be returned as a string, not raised."""
    mcp, connection = _setup_router()
    connection.send_command.side_effect = ConnectionError("TCP refused")

    result = mcp.tools["statetree_cmd"]("get_status", {})
    assert "Error" in result
    assert "TCP refused" in result
