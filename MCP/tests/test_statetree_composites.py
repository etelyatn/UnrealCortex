"""Tests for the StateTree composite MCP tool."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock


_MCP_SRC = Path(__file__).resolve().parents[1] / "src"
if str(_MCP_SRC) not in sys.path:
    sys.path.insert(0, str(_MCP_SRC))


def _extract_tool(connection):
    """Register the wrapper tool and return statetree_compose."""
    from cortex_mcp.tools.composites.statetree import register_statetree_compose_tools

    tools: dict[str, callable] = {}

    class MockMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                tools[name or fn.__name__] = fn
                return fn

            return decorator

    register_statetree_compose_tools(MockMCP(), connection)
    return tools["statetree_compose"]


def test_register_statetree_compose_tools_registers_tool():
    """The explicit wrapper module should register statetree_compose."""
    tool = _extract_tool(MagicMock())
    assert callable(tool)


def test_statetree_compose_create_flow_translates_commands():
    """Create flow should create, mutate, validate, and compile in order."""
    connection = MagicMock()
    connection.send_command.side_effect = [
        {
            "success": True,
            "data": {
                "asset_path": "/Game/StateTrees/ST_Test",
                "fingerprint": {"revision": 1},
            },
        },
        {
            "success": True,
            "data": {
                "state_id": "state-idle",
                "fingerprint": {"revision": 2},
            },
        },
        {
            "success": True,
            "data": {
                "transition_id": "transition-root-idle",
                "fingerprint": {"revision": 3},
            },
        },
        {
            "success": True,
            "data": {
                "validated": True,
                "fingerprint": {"revision": 4},
            },
        },
        {
            "success": True,
            "data": {
                "compiled": True,
                "fingerprint": {"revision": 5},
            },
        },
    ]

    tool = _extract_tool(connection)
    result = json.loads(
        tool(
            mode="create",
            asset_path="/Game/StateTrees/ST_Test",
            schema_class="/Script/GameplayStateTreeModule.StateTreeComponentSchema",
            states=[
                {
                    "name": "Idle",
                    "parent_state_path": "Root",
                    "type": "State",
                }
            ],
            transitions=[
                {
                    "source_state_path": "Root",
                    "target_state_path": "Root.Idle",
                    "trigger": "OnCompleted",
                }
            ],
        )
    )

    assert result["success"] is True
    assert result["asset_path"] == "/Game/StateTrees/ST_Test"

    calls = connection.send_command.call_args_list
    assert [call.args[0] for call in calls] == [
        "statetree.create_asset",
        "statetree.add_state",
        "statetree.add_transition",
        "statetree.validate_asset",
        "statetree.compile",
    ]
    assert calls[0].args[1] == {
        "asset_path": "/Game/StateTrees/ST_Test",
        "schema_class": "/Script/GameplayStateTreeModule.StateTreeComponentSchema",
        "save": False,
    }
    assert calls[1].args[1]["expected_fingerprint"] == {"revision": 1}
    assert calls[2].args[1]["expected_fingerprint"] == {"revision": 2}
    assert calls[3].args[1]["expected_fingerprint"] == {"revision": 3}
    assert calls[4].args[1]["expected_fingerprint"] == {"revision": 4}


def test_statetree_compose_update_requires_expected_fingerprint():
    """Update mode should reject missing stale-write guards before sending commands."""
    connection = MagicMock()
    tool = _extract_tool(connection)

    result = json.loads(
        tool(
            mode="update",
            asset_path="/Game/StateTrees/ST_Test",
            states=[{"name": "Idle"}],
        )
    )

    assert result["success"] is False
    assert "expected_fingerprint" in result["error"]
    connection.send_command.assert_not_called()


def test_statetree_compose_create_failure_attempts_cleanup():
    """Create failures should try to delete the partially created asset."""
    connection = MagicMock()
    connection.send_command.side_effect = [
        {
            "success": True,
            "data": {
                "asset_path": "/Game/StateTrees/ST_Test",
                "fingerprint": {"revision": 1},
            },
        },
        {
            "success": False,
            "error": "State insert failed",
        },
        {
            "success": True,
            "data": {},
        },
    ]

    tool = _extract_tool(connection)
    result = json.loads(
        tool(
            mode="create",
            asset_path="/Game/StateTrees/ST_Test",
            schema_class="/Script/GameplayStateTreeModule.StateTreeComponentSchema",
            states=[{"name": "Idle"}],
        )
    )

    assert result["success"] is False
    assert "cleanup" in result

    calls = connection.send_command.call_args_list
    assert [call.args[0] for call in calls] == [
        "statetree.create_asset",
        "statetree.add_state",
        "statetree.delete_asset",
    ]
    assert calls[2].args[1] == {
        "asset_path": "/Game/StateTrees/ST_Test",
        "force": True,
        "expected_fingerprint": {"revision": 1},
    }
