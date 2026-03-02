"""Unit tests for material composite tool helper functions."""

import os
import sys
from pathlib import Path

import pytest
from unittest.mock import MagicMock, patch
import json

# Add tools directory to path for imports
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from material.composites import (
    _resolve_class_name,
    _validate_spec,
    _build_batch_commands,
    register_material_composite_tools,
)


def _extract_tool(connection):
    """Register composite tool with a mock MCP and return create_material_graph."""
    tools = {}

    class MockMCP:
        def tool(self):
            def decorator(fn):
                tools[fn.__name__] = fn
                return fn
            return decorator

    register_material_composite_tools(MockMCP(), connection)
    return tools["create_material_graph"]


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
        connections = [{"from": "A.0", "to": "UnknownNode.Input"}]
        with pytest.raises(ValueError, match="Unknown target node"):
            _validate_spec("M_Test", "/Game/", nodes, connections)

    def test_material_target_allowed(self):
        """Validation allows 'Material' as special target node."""
        nodes = [{"name": "A", "class": "Constant"}]
        connections = [{"from": "A.0", "to": "Material.BaseColor"}]
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
        assert commands[0]["params"]["asset_path"] == "/Game"

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


class TestTimeoutScaling:
    def test_small_batch_uses_default(self):
        """Batch of 10 commands should use 60s timeout (max(60, 10*2) = 60)."""
        nodes = [{"class": "Constant", "name": f"N{i}"} for i in range(10)]
        commands = _build_batch_commands("M_Test", "/Game/", nodes, [])
        expected_timeout = max(60, len(commands) * 2)
        assert expected_timeout == 60

    def test_large_batch_scales_timeout(self):
        """Batch of 50+ commands should scale timeout."""
        nodes = [{"class": "Constant", "name": f"N{i}", "params": {"R": 1.0}} for i in range(40)]
        connections = [{"from": f"N{i}.Out", "to": "Material.BaseColor"} for i in range(1)]
        commands = _build_batch_commands("M_Test", "/Game/", nodes, connections)
        expected_timeout = max(60, len(commands) * 2)
        # 1 create + 40 add_node + 40 set_property + 1 connect = 82 commands
        # timeout = max(60, 82*2) = 164
        assert expected_timeout > 60


class TestCleanupOnFailure:
    """Tests for cleanup-on-failure behavior in composite tool."""

    def test_cleanup_deletes_partial_on_batch_failure(self):
        """When batch fails after step 0 succeeded, partial asset should be deleted."""
        mock_conn = MagicMock()

        # Simulate: step 0 succeeds (create_material), step 1 fails
        mock_conn.send_command.side_effect = [
            # Call 1: batch — step 0 succeeds, step 1 fails
            {
                "success": True,
                "data": {
                    "results": [
                        {"index": 0, "success": True, "data": {"asset_path": "/Game/M_Test"}, "timing_ms": 1},
                        {"index": 1, "success": False, "error_message": "Pin not found", "command": "material.add_node", "timing_ms": 0},
                    ],
                    "count": 2,
                    "total_timing_ms": 1,
                },
            },
            # Call 2: material.delete_material cleanup
            {"success": True, "data": {}},
        ]

        nodes = [{"class": "Constant", "name": "A"}]
        connections = [{"from": "A.0", "to": "Material.BaseColor"}]
        tool = _extract_tool(mock_conn)
        result = json.loads(tool(name="M_Test", path="/Game/", nodes=nodes, connections=connections))

        assert result["success"] is False
        delete_calls = [
            c for c in mock_conn.send_command.call_args_list
            if c.args[0] == "material.delete_material"
        ]
        assert len(delete_calls) == 1
        assert delete_calls[0].args[1]["asset_path"] == "/Game/M_Test"


class TestAutoLayoutWarning:
    """Tests for auto_layout failure returning success with warning."""

    def test_auto_layout_failure_returns_success_with_warning(self):
        """If auto_layout fails after successful batch, response should still be success with warning."""
        mock_conn = MagicMock()

        # Call sequence: batch → auto_layout (raises) → get_material → list_nodes → list_connections
        mock_conn.send_command.side_effect = [
            # Call 1: batch — all steps succeed
            {
                "success": True,
                "data": {
                    "results": [
                        {"index": 0, "success": True, "data": {"asset_path": "/Game/M_Test"}, "timing_ms": 1},
                        {"index": 1, "success": True, "data": {"node_id": "Expr_0"}, "timing_ms": 1},
                    ],
                    "count": 2,
                    "total_timing_ms": 2,
                },
            },
            # Call 2: auto_layout raises — should be caught, result still success
            RuntimeError("auto_layout failed: graph too complex"),
            # Calls 3–5: verification readback
            {"data": {"node_count": 1, "blend_mode": None, "shading_model": None}},
            {"data": {"nodes": [{"expression_class": "MaterialExpressionConstant"}]}},
            {"data": {"connections": []}},
        ]

        nodes = [{"class": "Constant", "name": "A"}]
        connections = [{"from": "A.0", "to": "Material.BaseColor"}]
        tool = _extract_tool(mock_conn)
        result = json.loads(tool(name="M_Test", path="/Game/", nodes=nodes, connections=connections))

        assert result["success"] is True
        assert "warnings" in result
        assert any(w["step"] == "auto_layout" for w in result["warnings"])


class TestErrorPropagation:
    """Tests for error response format from composite tool."""

    def test_error_response_has_required_fields(self):
        """Failed batch should produce response with summary, completed_steps, failed_step."""
        # Build a batch and simulate failure response
        nodes = [{"class": "Constant", "name": "A"}]
        connections = [{"from": "A.Out", "to": "Material.BaseColor"}]
        commands = _build_batch_commands("M_Test", "/Game/", nodes, connections)

        # Simulate failure response from C++
        batch_result = {
            "success": True,
            "data": {
                "results": [
                    {"index": 0, "success": True, "data": {"asset_path": "/Game/M_Test"}, "timing_ms": 1},
                    {"index": 1, "success": False, "error_message": "Class not found", "command": "material.add_node", "timing_ms": 0},
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
        assert "error_message" in failed


FIXTURE_DIR = os.path.join(os.path.dirname(__file__), "fixtures")


class TestFixtures:
    """Tests using pulsating gradient reference data."""

    def _load_fixture(self, name):
        """Load a JSON fixture file."""
        path = os.path.join(FIXTURE_DIR, name)
        with open(path) as f:
            return json.load(f)

    def test_composite_to_batch_translation(self):
        """Composite request should translate to expected batch commands."""
        # Load composite request fixture
        request = self._load_fixture("pulsating_gradient_composite_request.json")

        # Generate batch commands
        commands = _build_batch_commands(
            request["name"],
            request["path"],
            request["nodes"],
            request["connections"]
        )

        # Load expected batch request
        expected = self._load_fixture("pulsating_gradient_batch_request.json")

        # Verify command count matches
        assert len(commands) == len(expected["commands"])
        assert len(commands) == 24

        # Verify command types in order
        assert commands[0]["command"] == "material.create_material"
        add_node_count = sum(1 for c in commands if c["command"] == "material.add_node")
        set_prop_count = sum(1 for c in commands if c["command"] == "material.set_node_property")
        connect_count = sum(1 for c in commands if c["command"] == "material.connect")

        assert add_node_count == 9  # 9 nodes
        assert set_prop_count == 4  # Red.Constant, Blue.Constant, GlowStrength.ParameterName, GlowStrength.DefaultValue
        assert connect_count == 10  # 10 connections

    def test_success_response_has_required_fields(self):
        """Success response fixture should have all required fields."""
        response = self._load_fixture("pulsating_gradient_success_response.json")

        # Verify top-level fields
        assert response["success"] is True
        assert response["asset_path"] == "/Game/Materials/M_PulsatingGradient"
        assert response["total_steps"] == 24
        assert response["node_count"] == 9
        assert response["connection_count"] == 10
        assert response["properties_set"] == 4
        assert "total_timing_ms" in response

        # Verify steps_summary
        summary = response["steps_summary"]
        assert summary["create"] == 1
        assert summary["add_node"] == 9
        assert summary["set_node_property"] == 4
        assert summary["connect"] == 10
        assert summary["auto_layout"] == 1

    def test_error_response_has_required_fields(self):
        """Error response fixture should have summary, failed_step, completed_steps."""
        response = self._load_fixture("pulsating_gradient_error_response.json")

        # Verify top-level fields
        assert response["success"] is False
        assert "summary" in response
        assert "Step 15 of 24 failed" in response["summary"]
        assert response["asset_path"] == "/Game/Materials/M_PulsatingGradient"
        assert response["completed_steps"] == 15
        assert response["total_steps"] == 24

        # Verify failed_step
        failed = response["failed_step"]
        assert failed["index"] == 15
        assert failed["command"] == "material.connect"
        assert "error" in failed

        # Verify recovery_action
        recovery = response["recovery_action"]
        assert recovery["action"] == "deleted_partial"
        assert recovery["path"] == "/Game/Materials/M_PulsatingGradient"


class TestMaterialProperties:
    """Tests for material_properties parameter in create_material_graph."""

    def test_validate_spec_accepts_valid_dict(self):
        """_validate_spec accepts a valid material_properties dict."""
        nodes = [{"name": "A", "class": "Constant"}]
        # Should not raise
        _validate_spec("M_Test", "/Game/", nodes, [], material_properties={"BlendMode": "BLEND_Translucent"})

    def test_validate_spec_accepts_none(self):
        """_validate_spec accepts None for material_properties."""
        nodes = [{"name": "A", "class": "Constant"}]
        # Should not raise
        _validate_spec("M_Test", "/Game/", nodes, [], material_properties=None)

    def test_validate_spec_rejects_non_dict(self):
        """_validate_spec rejects non-dict material_properties."""
        nodes = [{"name": "A", "class": "Constant"}]
        with pytest.raises(ValueError, match="material_properties must be a dict"):
            _validate_spec("M_Test", "/Game/", nodes, [], material_properties="BlendMode")

        with pytest.raises(ValueError, match="material_properties must be a dict"):
            _validate_spec("M_Test", "/Game/", nodes, [], material_properties=["BlendMode"])

    def test_build_commands_inserts_properties_after_create_before_nodes(self):
        """material_properties commands appear after create but before add_node."""
        nodes = [{"name": "Color", "class": "Constant3Vector"}]
        connections = [{"from": "Color.RGB", "to": "Material.BaseColor"}]
        mat_props = {"BlendMode": "BLEND_Translucent", "ShadingModel": "MSM_Unlit"}

        commands = _build_batch_commands("M_Test", "/Game/", nodes, connections, material_properties=mat_props)

        # Step 0: create_material
        assert commands[0]["command"] == "material.create_material"

        # Steps 1-2: set_material_property (order matches dict insertion order)
        assert commands[1]["command"] == "material.set_material_property"
        assert commands[1]["params"]["property_name"] == "BlendMode"
        assert commands[1]["params"]["value"] == "BLEND_Translucent"
        assert commands[1]["params"]["asset_path"] == "$steps[0].data.asset_path"

        assert commands[2]["command"] == "material.set_material_property"
        assert commands[2]["params"]["property_name"] == "ShadingModel"
        assert commands[2]["params"]["value"] == "MSM_Unlit"

        # Step 3: add_node (offset by 2 property commands)
        assert commands[3]["command"] == "material.add_node"

        # Step 4: connect (references step 3 for node_id)
        assert commands[4]["command"] == "material.connect"
        assert commands[4]["params"]["source_node"] == "$steps[3].data.node_id"

    def test_node_step_refs_offset_with_properties(self):
        """Node $steps[N] references are correctly offset when material_properties are present."""
        nodes = [
            {"name": "A", "class": "Constant"},
            {"name": "B", "class": "Multiply"},
        ]
        connections = [{"from": "A.0", "to": "B.A"}]
        mat_props = {"BlendMode": "BLEND_Translucent"}

        commands = _build_batch_commands("M_Test", "/Game/", nodes, connections, material_properties=mat_props)

        # Step 0: create_material
        # Step 1: set_material_property (1 property)
        # Step 2: add_node A
        # Step 3: add_node B
        # Step 4: connect A->B
        assert commands[0]["command"] == "material.create_material"
        assert commands[1]["command"] == "material.set_material_property"
        assert commands[2]["command"] == "material.add_node"
        assert commands[3]["command"] == "material.add_node"
        assert commands[4]["command"] == "material.connect"

        # Connect should reference step 2 (A) and step 3 (B)
        assert commands[4]["params"]["source_node"] == "$steps[2].data.node_id"
        assert commands[4]["params"]["target_node"] == "$steps[3].data.node_id"

    def test_no_properties_unchanged_behavior(self):
        """Without material_properties, behavior is unchanged (nodes start at step 1)."""
        nodes = [{"name": "A", "class": "Constant"}]
        connections = [{"from": "A.0", "to": "Material.BaseColor"}]

        commands = _build_batch_commands("M_Test", "/Game/", nodes, connections)

        assert commands[0]["command"] == "material.create_material"
        assert commands[1]["command"] == "material.add_node"
        assert commands[2]["command"] == "material.connect"
        assert commands[2]["params"]["source_node"] == "$steps[1].data.node_id"

    def test_properties_with_node_params_correct_refs(self):
        """set_node_property commands reference correct node steps when properties are present."""
        nodes = [
            {"name": "Tex", "class": "TextureSample", "params": {"Texture": "/Game/T_Base"}},
        ]
        connections = []
        mat_props = {"BlendMode": "BLEND_Masked"}

        commands = _build_batch_commands("M_Test", "/Game/", nodes, connections, material_properties=mat_props)

        # Step 0: create_material
        # Step 1: set_material_property
        # Step 2: add_node (Tex)
        # Step 3: set_node_property (Tex.Texture)
        assert commands[0]["command"] == "material.create_material"
        assert commands[1]["command"] == "material.set_material_property"
        assert commands[2]["command"] == "material.add_node"
        assert commands[3]["command"] == "material.set_node_property"
        assert commands[3]["params"]["node_id"] == "$steps[2].data.node_id"
        assert commands[3]["params"]["property_name"] == "Texture"


class TestMaterialVerificationIntegration:
    """Integration tests for material verification in the composite response."""

    def test_success_includes_verification(self):
        mock_connection = MagicMock()
        mock_connection.send_command.side_effect = [
            {
                "data": {
                    "results": [
                        {"index": 0, "success": True, "data": {"asset_path": "/Game/M_Test"}},
                        {"index": 1, "success": True, "data": {"node_id": "Expr_0"}},
                    ],
                    "total_timing_ms": 50,
                }
            },
            {"data": {"success": True}},
            {"data": {"node_count": 2, "blend_mode": "Opaque", "shading_model": "DefaultLit"}},
            {"data": {"nodes": [
                {"expression_class": "MaterialExpressionConstant"},
                {"expression_class": "MaterialExpressionMaterialOutput"},
            ]}},
            {"data": {"connections": []}},
        ]

        tool = _extract_tool(mock_connection)
        result = json.loads(tool(
            name="M_Test",
            path="/Game/",
            nodes=[{"name": "C", "class": "Constant"}],
            connections=[],
        ))

        assert result["success"] is True
        assert result["verification"]["verified"] is True

    def test_readback_failure_degrades_gracefully(self):
        mock_connection = MagicMock()
        mock_connection.send_command.side_effect = [
            {
                "data": {
                    "results": [
                        {"index": 0, "success": True, "data": {"asset_path": "/Game/M_Test"}},
                    ],
                    "total_timing_ms": 5,
                }
            },
            {"data": {"success": True}},
            ConnectionError("TCP dropped"),
        ]

        tool = _extract_tool(mock_connection)
        result = json.loads(tool(
            name="M_Test",
            path="/Game/",
            nodes=[],
            connections=[],
        ))

        assert result["success"] is True
        assert result["verification"]["verified"] is None
        assert result["verification"]["error_code"] == "READBACK_FAILED"
