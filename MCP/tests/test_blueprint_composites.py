"""Unit tests for Blueprint composite tool helper functions."""

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
