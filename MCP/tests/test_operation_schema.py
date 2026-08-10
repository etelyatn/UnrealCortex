"""Tests for profile-aware operation schemas with a bounded retry budget."""
import json
from unittest.mock import MagicMock

import pytest

from cortex_mcp.operation_schema import (
    RETRY_BUDGET_EXHAUSTED,
    build_profile_operation_schema,
    reset_operation_schema_budget,
)
from cortex_mcp.tcp_client import UECommandError


def _live_editor_connection(command="describe_node"):
    connection = MagicMock()
    connection.send_command.return_value = {
        "success": True,
        "data": {
            "source": "live_editor",
            "editor_instance_id": "instance-1",
            "plugin_build_id": "0.1.13-24601",
            "domain": "graph",
            "command": command,
            "router": "graph_cmd",
            "params": [{"name": "node_class", "type": "string", "required": True}],
        },
    }
    return connection


def test_profile_allows_umg_authoring_graph_command():
    reset_operation_schema_budget()
    connection = _live_editor_connection()
    payload = json.loads(build_profile_operation_schema(connection, "UMGAuthoring", "graph", "describe_node"))
    assert payload["source"] == "live_editor"
    assert payload["editor_available"] is True
    assert payload["policy_allowed"] is True
    assert payload["execution_shape"] == {"type": "router", "tool": "graph_cmd"}
    assert "budget_remaining" in payload


def test_profile_blocks_blueprint_add_variable_even_when_editor_advertises():
    reset_operation_schema_budget()
    connection = _live_editor_connection(command="add_variable")
    payload = json.loads(build_profile_operation_schema(connection, "UMGAuthoring", "blueprint", "add_variable"))
    assert payload["editor_available"] is True
    assert payload["policy_allowed"] is False
    assert "blocked_reason" in payload
    assert "blueprint.add_variable" in payload["blocked_reason"]
    assert "suggested_next_action" in payload


def test_editor_missing_command_reports_restart_guidance():
    reset_operation_schema_budget()
    connection = MagicMock()
    connection.send_command.side_effect = UECommandError(
        "core.get_operation_schema", "CAPABILITY_COMMAND_NOT_FOUND",
        "Command not registered by live editor",
        {"cache_advertised": True, "restart_or_reload_required": True},
    )
    with pytest.MonkeyPatch.context() as mp:
        from cortex_mcp import capabilities as caps_mod
        mp.setattr(caps_mod, "load_capabilities_cache",
                   lambda: {"domains": {"graph": {"commands": [{"name": "describe_node"}]}}})
        payload = json.loads(build_profile_operation_schema(connection, "UMGAuthoring", "graph", "describe_node"))
    assert payload["editor_available"] is False
    assert payload["policy_allowed"] is False
    assert payload["restart_or_reload_required"] is True
    assert payload["suggested_next_action"].startswith("Restart")


def test_retry_budget_exhausted_after_declared_corrections():
    reset_operation_schema_budget()
    connection = MagicMock()
    connection.send_command.side_effect = UECommandError(
        "core.get_operation_schema", "CAPABILITY_COMMAND_NOT_FOUND",
        "missing", {"cache_advertised": False, "restart_or_reload_required": False},
    )
    first = json.loads(build_profile_operation_schema(connection, "UMGAuthoring", "graph", "describe_node"))
    second = json.loads(build_profile_operation_schema(connection, "UMGAuthoring", "graph", "describe_node"))
    third = json.loads(build_profile_operation_schema(connection, "UMGAuthoring", "graph", "describe_node"))
    assert "budget_remaining" in first
    assert "budget_remaining" in second
    assert third.get("_error") == RETRY_BUDGET_EXHAUSTED
