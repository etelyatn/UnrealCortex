"""Unit tests for cleanup_blueprint_migration with migrated_overrides."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock, call

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
    mcp = MockMCP()
    connection = MagicMock()
    register_blueprint_analysis_tools(mcp, connection)
    return mcp, connection


def _node(node_id: str, cls: str, display_name: str) -> dict:
    return {"node_id": node_id, "class": cls, "display_name": display_name}


def _list_nodes_response(*nodes) -> dict:
    return {"data": {"nodes": list(nodes)}}


# --- No migrated_overrides: unchanged behavior ---

def test_no_overrides_passes_compile_flag_directly():
    """Without migrated_overrides, compile=True is passed to bp.cleanup_migration."""
    mcp, connection = _setup()
    connection.send_command.return_value = {"data": {"asset_path": "/Game/BP_Test"}}

    mcp.tools["cleanup_blueprint_migration"](asset_path="/Game/BP_Test")

    connection.send_command.assert_called_once_with(
        "blueprint.cleanup_migration",
        {"asset_path": "/Game/BP_Test", "compile": True},
    )


def test_no_overrides_does_not_call_list_nodes():
    """Without migrated_overrides, graph.find_event_handler is never called."""
    mcp, connection = _setup()
    connection.send_command.return_value = {"data": {}}

    mcp.tools["cleanup_blueprint_migration"](asset_path="/Game/BP_Test")

    calls = [c.args[0] for c in connection.send_command.call_args_list]
    assert "graph.find_event_handler" not in calls


# --- Known override: full prune flow ---

def test_known_override_defers_compile_in_cleanup_migration():
    """With migrated_overrides, bp.cleanup_migration receives compile=False."""
    mcp, connection = _setup()
    connection.send_command.return_value = {"data": {}}

    mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
    )

    cleanup_call = next(
        c for c in connection.send_command.call_args_list
        if c.args[0] == "blueprint.cleanup_migration"
    )
    assert cleanup_call.args[1]["compile"] is False


def test_known_override_calls_list_nodes():
    """With migrated_overrides, graph.find_event_handler is called on EventGraph."""
    mcp, connection = _setup()
    connection.send_command.return_value = {"data": {"nodes": []}}

    mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
    )

    connection.send_command.assert_any_call(
        "graph.find_event_handler",
        {"asset_path": "/Game/BP_Test", "event_name": "Event BeginPlay"},
    )


def test_known_override_removes_matching_event_node():
    """Matching UK2Node_Event with correct display_name is removed."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-aaa", "UK2Node_Event", "Event BeginPlay"),
                _node("node-bbb", "UK2Node_CallFunction", "Print String"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
    )

    connection.send_command.assert_any_call(
        "graph.remove_node",
        {"asset_path": "/Game/BP_Test", "node_id": "node-aaa", "graph_name": "EventGraph"},
    )


def test_known_override_does_not_remove_non_event_node():
    """Non-event node with matching display_name is not removed."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-aaa", "UK2Node_CallFunction", "Event BeginPlay"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
    )

    calls = [c.args[0] for c in connection.send_command.call_args_list]
    assert "graph.remove_node" not in calls


def test_known_override_calls_delete_orphaned_nodes():
    """After removing an event node, bp.delete_orphaned_nodes is called."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-aaa", "UK2Node_Event", "Event BeginPlay"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
    )

    connection.send_command.assert_any_call(
        "blueprint.delete_orphaned_nodes",
        {"asset_path": "/Game/BP_Test", "graph_name": "EventGraph", "compile": True},
    )


def test_delete_orphaned_nodes_respects_compile_false():
    """compile=False is forwarded to bp.delete_orphaned_nodes when nodes were removed."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-aaa", "UK2Node_Event", "Event BeginPlay"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
        compile=False,
    )

    connection.send_command.assert_any_call(
        "blueprint.delete_orphaned_nodes",
        {"asset_path": "/Game/BP_Test", "graph_name": "EventGraph", "compile": False},
    )


def test_known_override_result_includes_pruned_count():
    """Result includes pruned_event_nodes count."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-aaa", "UK2Node_Event", "Event BeginPlay"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    result = mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
    )
    parsed = json.loads(result)

    assert parsed["pruned_event_nodes"] == 1
    assert "Event BeginPlay" in parsed["pruned_event_node_names"]


# --- Unknown override name: graceful skip ---

def test_unknown_override_does_not_raise():
    """Unknown C++ override name is silently skipped — no remove, no orphan cleanup."""
    mcp, connection = _setup()
    connection.send_command.return_value = {"data": {}}

    result = mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["SomeCustomMethod"],
    )
    parsed = json.loads(result)

    assert "error" not in parsed
    assert parsed["pruned_event_nodes"] == 0

    calls = [c.args[0] for c in connection.send_command.call_args_list]
    assert "blueprint.delete_orphaned_nodes" not in calls


def test_unknown_override_no_remove_node_called():
    """Unknown override skips list_nodes, remove_node, and delete_orphaned_nodes."""
    mcp, connection = _setup()
    connection.send_command.return_value = {"data": {}}

    mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["SomeCustomMethod"],
    )

    calls = [c.args[0] for c in connection.send_command.call_args_list]
    assert "graph.find_event_handler" not in calls
    assert "graph.remove_node" not in calls
    assert "blueprint.delete_orphaned_nodes" not in calls


def test_unknown_override_still_compiles():
    """Unknown override still triggers bp.compile since cleanup_migration deferred it."""
    mcp, connection = _setup()
    connection.send_command.return_value = {"data": {}}

    mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["SomeCustomMethod"],
        compile=True,
    )

    connection.send_command.assert_any_call(
        "blueprint.compile",
        {"asset_path": "/Game/BP_Test"},
    )


def test_no_matching_node_still_compiles():
    """Known override with no matching node in EventGraph triggers bp.compile."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-bbb", "UK2Node_CallFunction", "Print String"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
        compile=True,
    )

    connection.send_command.assert_any_call(
        "blueprint.compile",
        {"asset_path": "/Game/BP_Test"},
    )


# --- No matching node in EventGraph: graceful skip ---

def test_no_matching_event_node_returns_zero_pruned():
    """Known override but no matching node → pruned_event_nodes=0."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-bbb", "UK2Node_CallFunction", "Print String"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    result = mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
    )
    parsed = json.loads(result)

    assert parsed["pruned_event_nodes"] == 0
    assert parsed["pruned_event_node_names"] == []


# --- Multiple overrides ---

def test_multiple_overrides_remove_all_matching_nodes():
    """Two overrides → two event nodes removed."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-aaa", "UK2Node_Event", "Event BeginPlay"),
                _node("node-bbb", "UK2Node_Event", "Event Tick"),
                _node("node-ccc", "UK2Node_CallFunction", "Print String"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    result = mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay", "Tick"],
    )
    parsed = json.loads(result)

    assert parsed["pruned_event_nodes"] == 2

    removed_node_calls = [
        c for c in connection.send_command.call_args_list
        if c.args[0] == "graph.remove_node"
    ]
    removed_ids = {c.args[1]["node_id"] for c in removed_node_calls}
    assert removed_ids == {"node-aaa", "node-bbb"}


# --- Overlap override mapping ---

def test_notify_actor_begin_overlap_maps_to_event_actor_begin_overlap():
    """NotifyActorBeginOverlap maps to 'Event ActorBeginOverlap' display name."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-aaa", "UK2Node_Event", "Event ActorBeginOverlap"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    result = mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["NotifyActorBeginOverlap"],
    )
    parsed = json.loads(result)

    assert parsed["pruned_event_nodes"] == 1


# --- Original cleanup params still forwarded ---

def test_remove_variables_forwarded_to_cleanup_migration():
    """remove_variables param is forwarded to bp.cleanup_migration."""
    mcp, connection = _setup()
    connection.send_command.return_value = {"data": {}}

    mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        remove_variables=["Health", "Speed"],
        migrated_overrides=["BeginPlay"],
    )

    cleanup_call = next(
        c for c in connection.send_command.call_args_list
        if c.args[0] == "blueprint.cleanup_migration"
    )
    assert cleanup_call.args[1]["remove_variables"] == ["Health", "Speed"]


# --- EndPlay display name has a space ---

def test_end_play_maps_to_event_end_play_with_space():
    """EndPlay maps to 'Event End Play' (with space), matching UE DisplayName metadata."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-aaa", "UK2Node_Event", "Event End Play"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    result = mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["EndPlay"],
    )
    parsed = json.loads(result)

    assert parsed["pruned_event_nodes"] == 1


# --- unrecognized_overrides field ---

def test_unknown_override_reported_in_unrecognized_overrides():
    """Unknown override name appears in unrecognized_overrides result field."""
    mcp, connection = _setup()
    connection.send_command.return_value = {"data": {}}

    result = mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["SomeCustomMethod"],
    )
    parsed = json.loads(result)

    assert parsed.get("unrecognized_overrides") == ["SomeCustomMethod"]


def test_mixed_known_and_unknown_overrides():
    """Known override is pruned; unknown override is reported in unrecognized_overrides."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-aaa", "UK2Node_Event", "Event BeginPlay"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    result = mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay", "SomeCustomMethod"],
    )
    parsed = json.loads(result)

    assert parsed["pruned_event_nodes"] == 1
    assert parsed.get("unrecognized_overrides") == ["SomeCustomMethod"]


def test_all_known_overrides_no_unrecognized_field():
    """When all overrides are known, unrecognized_overrides is absent from result."""
    mcp, connection = _setup()
    connection.send_command.return_value = {"data": {"nodes": []}}

    result = mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
    )
    parsed = json.loads(result)

    assert "unrecognized_overrides" not in parsed


# --- compile=False + no matching node ---

def test_no_matching_node_compile_false_does_not_compile():
    """Known override with no matching node and compile=False does not call bp.compile."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return _list_nodes_response(
                _node("node-bbb", "UK2Node_CallFunction", "Print String"),
            )
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
        compile=False,
    )

    calls = [c.args[0] for c in connection.send_command.call_args_list]
    assert "blueprint.compile" not in calls


# --- Malformed node data (missing node_id) ---

def test_malformed_node_missing_node_id_is_skipped():
    """Node missing node_id key is skipped — no KeyError, no graph.remove_node call."""
    mcp, connection = _setup()

    def side_effect(command, params, **kwargs):
        if command == "graph.find_event_handler":
            return {"data": {"nodes": [
                {"class": "UK2Node_Event", "display_name": "Event BeginPlay"},  # no node_id
            ]}}
        return {"data": {}}

    connection.send_command.side_effect = side_effect

    result = mcp.tools["cleanup_blueprint_migration"](
        asset_path="/Game/BP_Test",
        migrated_overrides=["BeginPlay"],
    )
    parsed = json.loads(result)

    assert "error" not in parsed
    assert parsed["pruned_event_nodes"] == 0

    calls = [c.args[0] for c in connection.send_command.call_args_list]
    assert "graph.remove_node" not in calls

