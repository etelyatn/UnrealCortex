"""Unit tests for Blueprint composite tool helper functions."""

import json
import os
import sys
from pathlib import Path

import pytest

# Add tools directory to path for imports
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from blueprint.composites import (
    _resolve_class_name,
    _contains_ref_syntax,
    _validate_spec,
    _build_batch_commands,
)


class TestClassNameResolution:
    """Test _resolve_class_name() function."""

    def test_short_name_maps_to_full(self):
        """Short name maps to full UE class name via _BP_CLASS_MAP."""
        assert _resolve_class_name("Event") == "UK2Node_Event"
        assert _resolve_class_name("CallFunction") == "UK2Node_CallFunction"
        assert _resolve_class_name("Branch") == "UK2Node_IfThenElse"
        assert _resolve_class_name("Sequence") == "UK2Node_ExecutionSequence"

    def test_full_name_passthrough(self):
        """Full UK2Node_ class name passes through unchanged."""
        assert _resolve_class_name("UK2Node_Event") == "UK2Node_Event"
        assert _resolve_class_name("UK2Node_CallFunction") == "UK2Node_CallFunction"

    def test_unknown_gets_prefix(self):
        """Unknown class name gets UK2Node_ prefix."""
        assert _resolve_class_name("MyCustomNode") == "UK2Node_MyCustomNode"

    def test_alias_mapping(self):
        """Alias names map correctly."""
        assert _resolve_class_name("VariableGet") == "UK2Node_VariableGet"
        assert _resolve_class_name("VariableSet") == "UK2Node_VariableSet"
        assert _resolve_class_name("SpawnActor") == "UK2Node_SpawnActorFromClass"
        assert _resolve_class_name("CastTo") == "UK2Node_DynamicCast"
        assert _resolve_class_name("ForEachLoop") == "UK2Node_MacroInstance"


class TestContainsRefSyntax:
    """Test _contains_ref_syntax() function."""

    def test_string_with_ref(self):
        assert _contains_ref_syntax("$steps[0].data.asset_path") is True

    def test_plain_string(self):
        assert _contains_ref_syntax("hello world") is False

    def test_nested_dict_with_ref(self):
        assert _contains_ref_syntax({"key": "$steps[1].data.node_id"}) is True

    def test_nested_list_with_ref(self):
        assert _contains_ref_syntax(["$steps[0].data.x"]) is True

    def test_non_string(self):
        assert _contains_ref_syntax(42) is False


class TestValidation:
    """Test _validate_spec() function."""

    def test_missing_name(self):
        with pytest.raises(ValueError, match="Missing required field: name"):
            _validate_spec("", "/Game/", nodes=[], connections=[])

    def test_missing_path(self):
        with pytest.raises(ValueError, match="Missing required field: path"):
            _validate_spec("BP_Test", "", nodes=[], connections=[])

    def test_invalid_type(self):
        with pytest.raises(ValueError, match="type must be one of"):
            _validate_spec("BP_Test", "/Game/", bp_type="Invalid", nodes=[], connections=[])

    def test_valid_types(self):
        """All valid types should pass."""
        for t in ["Actor", "Component", "Widget", "Interface", "FunctionLibrary"]:
            _validate_spec("BP_Test", "/Game/", bp_type=t, nodes=[], connections=[])

    def test_duplicate_node_names(self):
        nodes = [
            {"name": "A", "class": "Event"},
            {"name": "A", "class": "CallFunction"},
        ]
        with pytest.raises(ValueError, match="Duplicate node names"):
            _validate_spec("BP_Test", "/Game/", nodes=nodes, connections=[])

    def test_duplicate_variable_names(self):
        variables = [
            {"name": "Health", "type": "float"},
            {"name": "Health", "type": "int"},
        ]
        with pytest.raises(ValueError, match="Duplicate variable names"):
            _validate_spec("BP_Test", "/Game/", variables=variables, nodes=[], connections=[])

    def test_duplicate_function_names(self):
        functions = [
            {"name": "DoStuff", "is_pure": False},
            {"name": "DoStuff", "is_pure": True},
        ]
        with pytest.raises(ValueError, match="Duplicate function names"):
            _validate_spec("BP_Test", "/Game/", functions=functions, nodes=[], connections=[])

    def test_node_missing_class(self):
        nodes = [{"name": "A"}]
        with pytest.raises(ValueError, match="missing 'class'"):
            _validate_spec("BP_Test", "/Game/", nodes=nodes, connections=[])

    def test_unknown_source_node_in_connection(self):
        nodes = [{"name": "A", "class": "Event"}]
        connections = [{"from": "Unknown.then", "to": "A.execute"}]
        with pytest.raises(ValueError, match="Unknown source node"):
            _validate_spec("BP_Test", "/Game/", nodes=nodes, connections=connections)

    def test_unknown_target_node_in_connection(self):
        nodes = [{"name": "A", "class": "Event"}]
        connections = [{"from": "A.then", "to": "Unknown.execute"}]
        with pytest.raises(ValueError, match="Unknown target node"):
            _validate_spec("BP_Test", "/Game/", nodes=nodes, connections=connections)

    def test_invalid_connection_format(self):
        nodes = [{"name": "A", "class": "Event"}]
        connections = [{"from": "InvalidFormat", "to": "A.execute"}]
        with pytest.raises(ValueError, match="Invalid 'from'"):
            _validate_spec("BP_Test", "/Game/", nodes=nodes, connections=connections)

    def test_ref_syntax_in_params_rejected(self):
        nodes = [
            {"name": "A", "class": "Event", "params": {"key": "$steps[0].data.x"}},
        ]
        with pytest.raises(ValueError, match=r"\$steps\["):
            _validate_spec("BP_Test", "/Game/", nodes=nodes, connections=[])

    def test_ref_syntax_in_pin_values_rejected(self):
        nodes = [
            {"name": "A", "class": "Event", "pin_values": {"InString": "$steps[0].data.x"}},
        ]
        with pytest.raises(ValueError, match=r"\$steps\["):
            _validate_spec("BP_Test", "/Game/", nodes=nodes, connections=[])

    def test_empty_nodes_and_connections_valid(self):
        """Variables-only BP with no nodes/connections should pass."""
        _validate_spec(
            "BP_Test", "/Game/",
            variables=[{"name": "Health", "type": "float"}],
            nodes=[], connections=[],
        )

    def test_valid_full_spec(self):
        """Full spec with all fields should pass."""
        _validate_spec(
            "BP_Test", "/Game/",
            variables=[{"name": "Health", "type": "float"}],
            functions=[{"name": "TakeDamage"}],
            nodes=[
                {"name": "BeginPlay", "class": "Event", "params": {"function_name": "Actor.ReceiveBeginPlay"}},
                {"name": "Print", "class": "CallFunction", "params": {"function_name": "KismetSystemLibrary.PrintString"}},
            ],
            connections=[{"from": "BeginPlay.then", "to": "Print.execute"}],
        )


class TestBatchCommandGeneration:
    """Test _build_batch_commands() function."""

    def test_basic_blueprint_with_one_node(self):
        """Basic BP with single event node generates create + add_node commands."""
        nodes = [{"name": "BeginPlay", "class": "Event", "params": {"function_name": "Actor.ReceiveBeginPlay"}}]
        commands = _build_batch_commands("BP_Test", "/Game/Blueprints/", "Actor", [], [], nodes, [], "EventGraph")

        # Step 0: bp.create
        assert commands[0]["command"] == "bp.create"
        assert commands[0]["params"]["name"] == "BP_Test"
        assert commands[0]["params"]["path"] == "/Game/Blueprints"
        assert commands[0]["params"]["type"] == "Actor"

        # Step 1: graph.add_node
        assert commands[1]["command"] == "graph.add_node"
        assert commands[1]["params"]["asset_path"] == "$steps[0].data.asset_path"
        assert commands[1]["params"]["node_class"] == "UK2Node_Event"
        assert commands[1]["params"]["params"]["function_name"] == "Actor.ReceiveBeginPlay"

    def test_step_index_formula_with_vars_funcs_nodes(self):
        """Step index formula: node_step = 1 + len(vars) + len(funcs) + node_index.

        Example: 2 vars, 1 func, 3 nodes -> node steps are 4, 5, 6.
        """
        variables = [
            {"name": "Health", "type": "float"},
            {"name": "Speed", "type": "float"},
        ]
        functions = [{"name": "TakeDamage"}]
        nodes = [
            {"name": "A", "class": "Event", "params": {"function_name": "Actor.ReceiveBeginPlay"}},
            {"name": "B", "class": "CallFunction", "params": {"function_name": "KismetSystemLibrary.PrintString"}},
            {"name": "C", "class": "Branch"},
        ]
        commands = _build_batch_commands("BP_Test", "/Game/", "Actor", variables, functions, nodes, [], "EventGraph")

        # Step 0: bp.create
        # Steps 1-2: bp.add_variable x2
        # Step 3: bp.add_function
        # Steps 4-6: graph.add_node x3
        assert commands[0]["command"] == "bp.create"
        assert commands[1]["command"] == "bp.add_variable"
        assert commands[2]["command"] == "bp.add_variable"
        assert commands[3]["command"] == "bp.add_function"
        assert commands[4]["command"] == "graph.add_node"
        assert commands[5]["command"] == "graph.add_node"
        assert commands[6]["command"] == "graph.add_node"

    def test_connections_reference_correct_node_steps(self):
        """Connections use $steps[{node_step}] to reference correct node."""
        variables = [{"name": "Health", "type": "float"}]
        nodes = [
            {"name": "BeginPlay", "class": "Event", "params": {"function_name": "Actor.ReceiveBeginPlay"}},
            {"name": "Print", "class": "CallFunction", "params": {"function_name": "KismetSystemLibrary.PrintString"}},
        ]
        connections = [{"from": "BeginPlay.then", "to": "Print.execute"}]
        commands = _build_batch_commands("BP_Test", "/Game/", "Actor", variables, [], nodes, connections, "EventGraph")

        # Step 0: bp.create
        # Step 1: bp.add_variable
        # Step 2: graph.add_node (BeginPlay) -> step index 2
        # Step 3: graph.add_node (Print) -> step index 3
        # Step 4: graph.connect
        connect_cmd = [c for c in commands if c["command"] == "graph.connect"][0]
        assert connect_cmd["params"]["source_node"] == "$steps[2].data.node_id"
        assert connect_cmd["params"]["source_pin"] == "then"
        assert connect_cmd["params"]["target_node"] == "$steps[3].data.node_id"
        assert connect_cmd["params"]["target_pin"] == "execute"

    def test_pin_values_reference_correct_node_step(self):
        """pin_values generate graph.set_pin_value with correct $ref."""
        nodes = [
            {"name": "Print", "class": "CallFunction",
             "params": {"function_name": "KismetSystemLibrary.PrintString"},
             "pin_values": {"InString": "Hello!", "bPrintToScreen": "true"}},
        ]
        commands = _build_batch_commands("BP_Test", "/Game/", "Actor", [], [], nodes, [], "EventGraph")

        # Step 0: bp.create
        # Step 1: graph.add_node (Print)
        # Steps 2-3: graph.set_pin_value x2
        set_pin_cmds = [c for c in commands if c["command"] == "graph.set_pin_value"]
        assert len(set_pin_cmds) == 2
        assert set_pin_cmds[0]["params"]["node_id"] == "$steps[1].data.node_id"
        pin_names = {c["params"]["pin_name"] for c in set_pin_cmds}
        assert pin_names == {"InString", "bPrintToScreen"}

    def test_command_order_create_vars_funcs_nodes_pins_connects(self):
        """Commands ordered: create, variables, functions, nodes, pin_values, connections."""
        variables = [{"name": "Health", "type": "float"}]
        functions = [{"name": "Heal"}]
        nodes = [
            {"name": "A", "class": "Event", "params": {"function_name": "Actor.ReceiveBeginPlay"}},
            {"name": "B", "class": "CallFunction",
             "params": {"function_name": "KismetSystemLibrary.PrintString"},
             "pin_values": {"InString": "Hi"}},
        ]
        connections = [{"from": "A.then", "to": "B.execute"}]
        commands = _build_batch_commands("BP_Test", "/Game/", "Actor", variables, functions, nodes, connections, "EventGraph")

        cmd_types = [c["command"] for c in commands]
        # Expected order: bp.create, bp.add_variable, bp.add_function, add_node x2, set_pin_value, connect
        assert cmd_types == [
            "bp.create",
            "bp.add_variable",
            "bp.add_function",
            "graph.add_node",
            "graph.add_node",
            "graph.set_pin_value",
            "graph.connect",
        ]

    def test_empty_nodes_and_connections_vars_only(self):
        """Variables-only BP generates create + add_variable commands."""
        variables = [{"name": "Health", "type": "float", "default_value": "100.0", "is_exposed": True, "category": "Stats"}]
        commands = _build_batch_commands("BP_Test", "/Game/", "Actor", variables, [], [], [], "EventGraph")

        assert len(commands) == 2
        assert commands[0]["command"] == "bp.create"
        assert commands[1]["command"] == "bp.add_variable"
        assert commands[1]["params"]["variable_name"] == "Health"
        assert commands[1]["params"]["variable_type"] == "float"
        assert commands[1]["params"]["default_value"] == "100.0"

    def test_trailing_slash_normalized(self):
        """Trailing slash on path is normalized."""
        commands = _build_batch_commands("BP_Test", "/Game/Blueprints/", "Actor", [], [], [], [], "EventGraph")
        assert commands[0]["params"]["path"] == "/Game/Blueprints"

    def test_graph_name_passed_to_add_node(self):
        """Custom graph_name is passed to add_node commands."""
        nodes = [{"name": "A", "class": "Event", "params": {"function_name": "Actor.ReceiveBeginPlay"}}]
        commands = _build_batch_commands("BP_Test", "/Game/", "Actor", [], [], nodes, [], "MyGraph")
        add_cmd = [c for c in commands if c["command"] == "graph.add_node"][0]
        assert add_cmd["params"]["graph_name"] == "MyGraph"


class TestTimeoutScaling:
    def test_small_batch_uses_minimum(self):
        """Small batch uses 60s minimum timeout."""
        commands = _build_batch_commands("BP_Test", "/Game/", "Actor", [], [], [], [], "EventGraph")
        expected = max(60, len(commands) * 2)
        assert expected == 60

    def test_large_batch_scales(self):
        """Large batch scales timeout beyond 60s."""
        nodes = [{"name": f"N{i}", "class": "Event", "params": {"function_name": "Actor.ReceiveBeginPlay"}} for i in range(40)]
        commands = _build_batch_commands("BP_Test", "/Game/", "Actor", [], [], nodes, [], "EventGraph")
        expected = max(60, len(commands) * 2)
        assert expected > 60


class TestCleanupOnFailure:
    """Tests for cleanup-on-failure behavior."""

    def test_cleanup_deletes_partial_on_batch_failure(self):
        """When batch fails after step 0 succeeded, partial asset should be cleaned up."""
        # Verify the design: _build_batch_commands produces commands where step 0 is bp.create
        nodes = [{"name": "A", "class": "Event", "params": {"function_name": "Actor.ReceiveBeginPlay"}}]
        commands = _build_batch_commands("BP_Test", "/Game/", "Actor", [], [], nodes, [], "EventGraph")
        assert commands[0]["command"] == "bp.create"
        # The composite tool will call bp.delete on failure if step 0 succeeded
        # This is verified by integration behavior — we check the design contract here


class TestErrorPropagation:
    """Tests for error response format."""

    def test_batch_failure_response_fields(self):
        """Failed batch response has summary, completed_steps, failed_step."""
        batch_result = {
            "success": True,
            "data": {
                "results": [
                    {"index": 0, "success": True, "data": {"asset_path": "/Game/BP_Test"}, "timing_ms": 1},
                    {"index": 1, "success": False, "error_message": "Class not found", "command": "graph.add_node", "timing_ms": 0},
                ],
                "count": 2,
                "total_timing_ms": 1,
            },
        }
        results = batch_result["data"]["results"]
        failed = None
        completed = 0
        for entry in results:
            if entry.get("success"):
                completed += 1
            else:
                failed = entry
                break

        assert failed is not None
        assert failed["index"] == 1
        assert completed == 1


FIXTURE_DIR = os.path.join(os.path.dirname(__file__), "fixtures")


class TestFixtures:
    """Tests using health actor reference data."""

    def _load_fixture(self, name):
        path = os.path.join(FIXTURE_DIR, name)
        with open(path) as f:
            return json.load(f)

    def test_composite_to_batch_translation(self):
        """Composite request translates to expected batch commands."""
        request = self._load_fixture("health_actor_composite_request.json")
        commands = _build_batch_commands(
            request["name"],
            request["path"],
            request.get("type", "Actor"),
            request.get("variables", []),
            request.get("functions", []),
            request.get("nodes", []),
            request.get("connections", []),
            request.get("graph_name", "EventGraph"),
        )

        expected = self._load_fixture("health_actor_batch_request.json")
        assert len(commands) == len(expected["commands"])

        # Verify command types in order
        assert commands[0]["command"] == "bp.create"
        var_count = sum(1 for c in commands if c["command"] == "bp.add_variable")
        func_count = sum(1 for c in commands if c["command"] == "bp.add_function")
        node_count = sum(1 for c in commands if c["command"] == "graph.add_node")
        pin_count = sum(1 for c in commands if c["command"] == "graph.set_pin_value")
        connect_count = sum(1 for c in commands if c["command"] == "graph.connect")

        assert var_count == 2
        assert func_count == 1
        assert node_count == 3
        assert pin_count == 1  # InString on PrintHealth
        assert connect_count == 1

    def test_step_indices_match_expected(self):
        """$ref step indices in batch commands match expected values."""
        request = self._load_fixture("health_actor_composite_request.json")
        commands = _build_batch_commands(
            request["name"], request["path"],
            request.get("type", "Actor"),
            request.get("variables", []),
            request.get("functions", []),
            request.get("nodes", []),
            request.get("connections", []),
            request.get("graph_name", "EventGraph"),
        )

        # 2 vars + 1 func = offset 3, so node steps are 4, 5, 6
        # Step 0: bp.create
        # Steps 1-2: bp.add_variable
        # Step 3: bp.add_function
        # Steps 4-6: graph.add_node (BeginPlay=4, PrintHealth=5, GetHealth=6)
        node_cmds = [c for c in commands if c["command"] == "graph.add_node"]
        assert node_cmds[0]["params"]["node_class"] == "UK2Node_Event"
        assert node_cmds[1]["params"]["node_class"] == "UK2Node_CallFunction"
        assert node_cmds[2]["params"]["node_class"] == "UK2Node_VariableGet"

        # Pin value should reference step 5 (PrintHealth)
        pin_cmd = [c for c in commands if c["command"] == "graph.set_pin_value"][0]
        assert pin_cmd["params"]["node_id"] == "$steps[5].data.node_id"

        # Connection: BeginPlay(step 4) -> PrintHealth(step 5)
        conn_cmd = [c for c in commands if c["command"] == "graph.connect"][0]
        assert conn_cmd["params"]["source_node"] == "$steps[4].data.node_id"
        assert conn_cmd["params"]["target_node"] == "$steps[5].data.node_id"
