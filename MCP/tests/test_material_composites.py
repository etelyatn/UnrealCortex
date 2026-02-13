"""Unit tests for material composite tool helper functions."""

import os
import sys
from pathlib import Path

import pytest

# Add tools directory to path for imports
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from material.composites import (
    _resolve_class_name,
    _validate_spec,
    _build_batch_commands,
)


class TestClassNameResolution:
    """Test _resolve_class_name() function."""

    def test_short_name_to_full(self):
        """Short name maps to full UE class name."""
        assert _resolve_class_name("TextureSample") == "MaterialExpressionTextureSample"
        assert _resolve_class_name("ScalarParameter") == "MaterialExpressionScalarParameter"
        assert _resolve_class_name("Multiply") == "MaterialExpressionMultiply"

    def test_full_name_passthrough(self):
        """Full class name passes through unchanged."""
        assert _resolve_class_name("MaterialExpressionCustom") == "MaterialExpressionCustom"
        assert _resolve_class_name("MaterialExpressionTextureSample") == "MaterialExpressionTextureSample"

    def test_unknown_gets_prefix(self):
        """Unknown class name gets MaterialExpression prefix."""
        assert _resolve_class_name("MyCustomNode") == "MaterialExpressionMyCustomNode"
        assert _resolve_class_name("FooBar") == "MaterialExpressionFooBar"

    def test_special_mapping(self):
        """Special mappings like Lerp → LinearInterpolate."""
        assert _resolve_class_name("Lerp") == "MaterialExpressionLinearInterpolate"
        assert _resolve_class_name("Desaturation") == "MaterialExpressionDesaturation"


class TestValidation:
    """Test _validate_spec() function."""

    def test_missing_name(self):
        """Validation fails when name is missing."""
        with pytest.raises(ValueError, match="Missing required field: name"):
            _validate_spec("", "/Game/", [], [])

    def test_missing_path(self):
        """Validation fails when path is missing."""
        with pytest.raises(ValueError, match="Missing required field: path"):
            _validate_spec("M_Test", "", [], [])

    def test_duplicate_node_names(self):
        """Validation fails on duplicate node names."""
        nodes = [
            {"name": "Node1", "class": "Constant"},
            {"name": "Node1", "class": "Multiply"},
        ]
        with pytest.raises(ValueError, match="Duplicate"):
            _validate_spec("M_Test", "/Game/", nodes, [])

    def test_unknown_source_node(self):
        """Validation fails when connection references unknown source node."""
        nodes = [{"name": "A", "class": "Constant"}]
        connections = [{"from": "B.Output", "to": "Material.BaseColor"}]
        with pytest.raises(ValueError, match="Unknown source node"):
            _validate_spec("M_Test", "/Game/", nodes, connections)

    def test_unknown_target_node(self):
        """Validation fails when connection references unknown target node."""
        nodes = [{"name": "A", "class": "Constant"}]
        connections = [{"from": "A.Output", "to": "UnknownNode.Input"}]
        with pytest.raises(ValueError, match="Unknown target node"):
            _validate_spec("M_Test", "/Game/", nodes, connections)

    def test_material_target_allowed(self):
        """Validation allows 'Material' as special target node."""
        nodes = [{"name": "A", "class": "Constant"}]
        connections = [{"from": "A.Output", "to": "Material.BaseColor"}]
        # Should not raise
        _validate_spec("M_Test", "/Game/", nodes, connections)

    def test_ref_syntax_in_params_rejected(self):
        """Validation rejects $steps[ in node params."""
        nodes = [
            {
                "name": "A",
                "class": "Constant",
                "params": {"Value": "$steps[0].data.something"},
            }
        ]
        with pytest.raises(ValueError, match=r"\$steps\["):
            _validate_spec("M_Test", "/Game/", nodes, [])

    def test_invalid_connection_format(self):
        """Validation fails when connection format is invalid."""
        nodes = [{"name": "A", "class": "Constant"}]
        connections = [{"from": "InvalidFormat", "to": "Material.BaseColor"}]
        with pytest.raises(ValueError, match="Invalid 'from'"):
            _validate_spec("M_Test", "/Game/", nodes, connections)


class TestBatchCommandGeneration:
    """Test _build_batch_commands() function."""

    def test_basic_material(self):
        """Basic material with single node and connection."""
        nodes = [{"name": "Color", "class": "Constant3Vector"}]
        connections = [{"from": "Color.RGB", "to": "Material.BaseColor"}]

        commands = _build_batch_commands("M_Test", "/Game/", nodes, connections)

        # Step 0: create_material
        assert commands[0]["command"] == "material.create_material"
        assert commands[0]["params"]["name"] == "M_Test"
        assert commands[0]["params"]["asset_path"] == "/Game/"

        # Step 1: add_node
        assert commands[1]["command"] == "material.add_node"
        assert commands[1]["params"]["asset_path"] == "$steps[0].data.asset_path"
        assert commands[1]["params"]["expression_class"] == "MaterialExpressionConstant3Vector"

        # Step 2: connect
        assert commands[2]["command"] == "material.connect"
        assert commands[2]["params"]["source_node"] == "$steps[1].data.node_id"
        assert commands[2]["params"]["source_output"] == "RGB"
        assert commands[2]["params"]["target_node"] == "MaterialResult"
        assert commands[2]["params"]["target_input"] == "BaseColor"

    def test_expression_to_expression_connection(self):
        """Connection between two expression nodes uses $ref for both."""
        nodes = [
            {"name": "A", "class": "Constant"},
            {"name": "B", "class": "Multiply"},
        ]
        connections = [{"from": "A.Output", "to": "B.A"}]

        commands = _build_batch_commands("M_Test", "/Game/", nodes, connections)

        # Find connect command
        connect = [c for c in commands if c["command"] == "material.connect"][0]
        assert connect["params"]["source_node"] == "$steps[1].data.node_id"
        assert connect["params"]["target_node"] == "$steps[2].data.node_id"

    def test_node_with_params_generates_set_property(self):
        """Nodes with params generate set_node_property commands."""
        nodes = [
            {
                "name": "Tex",
                "class": "TextureSample",
                "params": {"Texture": "/Game/T_Base", "SamplerType": "LinearColor"},
            }
        ]
        connections = []

        commands = _build_batch_commands("M_Test", "/Game/", nodes, connections)

        # Step 0: create_material
        assert commands[0]["command"] == "material.create_material"
        # Step 1: add_node
        assert commands[1]["command"] == "material.add_node"
        # Step 2,3: set_node_property (order not guaranteed)
        set_props = [c for c in commands if c["command"] == "material.set_node_property"]
        assert len(set_props) == 2
        assert set_props[0]["params"]["node_id"] == "$steps[1].data.node_id"
        assert set_props[1]["params"]["node_id"] == "$steps[1].data.node_id"

        prop_names = {p["params"]["property_name"] for p in set_props}
        assert prop_names == {"Texture", "SamplerType"}

    def test_command_order(self):
        """Commands are ordered: create, add_node, set_property, connect."""
        nodes = [
            {"name": "A", "class": "Constant", "params": {"R": 1.0}},
            {"name": "B", "class": "Multiply"},
        ]
        connections = [{"from": "A.Output", "to": "B.A"}]

        commands = _build_batch_commands("M_Test", "/Game/", nodes, connections)

        # Expected order:
        # 0: create_material
        # 1: add_node (A)
        # 2: add_node (B)
        # 3: set_node_property (A.R)
        # 4: connect
        assert commands[0]["command"] == "material.create_material"
        assert commands[1]["command"] == "material.add_node"
        assert commands[2]["command"] == "material.add_node"
        assert commands[3]["command"] == "material.set_node_property"
        assert commands[4]["command"] == "material.connect"

        # Verify step indices in set_property and connect
        assert commands[3]["params"]["node_id"] == "$steps[1].data.node_id"
        assert commands[4]["params"]["source_node"] == "$steps[1].data.node_id"
        assert commands[4]["params"]["target_node"] == "$steps[2].data.node_id"
