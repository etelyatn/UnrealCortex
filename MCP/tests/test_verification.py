"""Unit tests for verification module pure functions."""

import sys
from pathlib import Path

# Add source to path
src_dir = Path(__file__).parent.parent / "src"
sys.path.insert(0, str(src_dir))

from cortex_mcp.verification import VerificationResult
from cortex_mcp.verification.material import verify_material
from cortex_mcp.verification.blueprint import verify_blueprint
from cortex_mcp.verification.umg import verify_umg


class TestMaterialVerification:
    def _make_spec(self, nodes=None, connections=None, material_properties=None):
        return {
            "nodes": nodes or [],
            "connections": connections or [],
            "material_properties": material_properties or {},
        }

    def _make_readback(self, node_count=0, nodes=None, connections=None, blend_mode="Opaque", shading_model="DefaultLit"):
        return {
            "node_count": node_count,
            "nodes": nodes or [],
            "connections": connections or [],
            "blend_mode": blend_mode,
            "shading_model": shading_model,
        }

    def test_all_checks_pass(self):
        spec = self._make_spec(
            nodes=[
                {"name": "Tex", "class": "TextureSample"},
                {"name": "Coord", "class": "TextureCoordinate"},
            ],
            connections=[{"from": "Tex.RGB", "to": "Material.BaseColor"}],
            material_properties={"blend_mode": "Opaque"},
        )
        readback = self._make_readback(
            node_count=3,
            nodes=[
                {"expression_class": "MaterialExpressionTextureSample"},
                {"expression_class": "MaterialExpressionTextureCoordinate"},
                {"expression_class": "MaterialExpressionMaterialOutput"},
            ],
            connections=[
                {
                    "source_node": "MaterialExpressionTextureSample_0",
                    "source_output": 0,
                    "target_node": "MaterialResult",
                    "target_input": "BaseColor",
                }
            ],
        )
        result = verify_material(spec, readback)
        assert result.verified is True
        assert all(c.passed for c in result.checks.values())

    def test_empty_graph_detected(self):
        # When spec has nodes but readback returns 0 nodes, the asset registry
        # may not have synced yet — guard returns verified=None (inconclusive).
        spec = self._make_spec(nodes=[{"name": "Const", "class": "Constant"}])
        readback = self._make_readback(node_count=0, nodes=[])
        result = verify_material(spec, readback)
        assert result.verified is None
        assert result.message is not None

    def test_missing_node_class(self):
        spec = self._make_spec(nodes=[{"name": "Tex", "class": "TextureSample"}])
        readback = self._make_readback(node_count=2, nodes=[{"expression_class": "MaterialExpressionConstant"}])
        result = verify_material(spec, readback)
        assert result.verified is False
        assert result.checks["node_count:TextureSample"].passed is False

    def test_connection_count_mismatch(self):
        spec = self._make_spec(
            nodes=[{"name": "Tex", "class": "TextureSample"}],
            connections=[
                {"from": "Tex.RGB", "to": "Material.BaseColor"},
                {"from": "Tex.A", "to": "Material.Opacity"},
            ],
        )
        readback = self._make_readback(
            node_count=2,
            nodes=[{"expression_class": "MaterialExpressionTextureSample"}],
            connections=[
                {
                    "source_node": "MaterialExpressionTextureSample_0",
                    "source_output": 0,
                    "target_node": "MaterialResult",
                    "target_input": "BaseColor",
                }
            ],
        )
        result = verify_material(spec, readback)
        assert result.verified is False
        assert result.checks["connection_count"].passed is False

    def test_blend_mode_mismatch(self):
        spec = self._make_spec(material_properties={"blend_mode": "Translucent"})
        readback = self._make_readback(blend_mode="Opaque")
        result = verify_material(spec, readback)
        assert result.verified is False
        assert result.checks["blend_mode"].passed is False

    def test_no_property_check_when_not_in_spec(self):
        spec = self._make_spec(nodes=[{"name": "C", "class": "Constant"}])
        readback = self._make_readback(
            node_count=2,
            nodes=[{"expression_class": "MaterialExpressionConstant"}],
            blend_mode="Masked",
        )
        result = verify_material(spec, readback)
        assert "blend_mode" not in result.checks

    def test_to_dict_format(self):
        spec = self._make_spec(nodes=[{"name": "C", "class": "Constant"}])
        readback = self._make_readback(node_count=2, nodes=[{"expression_class": "MaterialExpressionConstant"}])
        result = verify_material(spec, readback)
        payload = result.to_dict()
        assert "verified" in payload
        assert "checks" in payload
        for check in payload["checks"].values():
            assert set(check.keys()) == {"expected", "actual", "pass"}

    def test_duplicate_spec_class_requires_matching_count(self):
        spec = self._make_spec(
            nodes=[
                {"name": "TexA", "class": "TextureSample"},
                {"name": "TexB", "class": "TextureSample"},
            ]
        )
        readback = self._make_readback(
            node_count=3,
            nodes=[
                {"expression_class": "MaterialExpressionTextureSample"},
                {"expression_class": "MaterialExpressionMaterialOutput"},
            ],
        )
        result = verify_material(spec, readback)
        assert result.verified is False
        assert result.checks["node_count:TextureSample"].passed is False

    def test_readback_failure_result(self):
        result = VerificationResult(verified=None, error_code="READBACK_FAILED", error="Connection timeout")
        payload = result.to_dict()
        assert payload["verified"] is None
        assert payload["error_code"] == "READBACK_FAILED"
        assert payload["error"] == "Connection timeout"
        assert "checks" not in payload


class TestBlueprintVerification:
    def _make_spec(self, variables=None, functions=None, nodes=None, graph_name="EventGraph"):
        return {
            "variables": variables or [],
            "functions": functions or [],
            "nodes": nodes or [],
            "graph_name": graph_name,
        }

    def _make_readback(self, is_compiled=True, variables=None, functions=None, graphs=None, nodes=None):
        return {
            "info": {
                "is_compiled": is_compiled,
                "variables": variables or [],
                "functions": functions or [],
                "graphs": graphs or [{"name": "EventGraph", "node_count": 0}],
            },
            "nodes": nodes or [],
        }

    def test_all_checks_pass(self):
        spec = self._make_spec(
            variables=[{"name": "Health"}],
            functions=[{"name": "TakeDamage"}],
            nodes=[{"name": "BeginPlay", "class": "Event"}],
        )
        readback = self._make_readback(
            is_compiled=True,
            variables=[{"name": "Health", "type": "Float"}],
            functions=[{"name": "TakeDamage"}],
            graphs=[{"name": "EventGraph", "node_count": 2}],
            nodes=[{"class": "K2Node_Event"}, {"class": "K2Node_CallFunction"}],
        )
        result = verify_blueprint(spec, readback)
        assert result.verified is True

    def test_compile_failed_maps_to_verification_failed(self):
        spec = self._make_spec()
        readback = self._make_readback(is_compiled=False)
        result = verify_blueprint(spec, readback)
        assert result.verified is False
        assert result.error_code == "COMPILE_FAILED"
        assert result.skipped is not None
        assert result.message is not None

    def test_missing_variable(self):
        spec = self._make_spec(variables=[{"name": "Health"}, {"name": "Speed"}])
        readback = self._make_readback(variables=[{"name": "Health"}], is_compiled=True)
        result = verify_blueprint(spec, readback)
        assert result.verified is False
        assert result.checks["variables_count"].passed is False

    def test_missing_function(self):
        spec = self._make_spec(functions=[{"name": "TakeDamage"}])
        readback = self._make_readback(functions=[], is_compiled=True)
        result = verify_blueprint(spec, readback)
        assert result.verified is False
        assert result.checks["functions_count"].passed is False

    def test_graph_missing(self):
        spec = self._make_spec(nodes=[{"name": "BeginPlay", "class": "Event"}], graph_name="MyGraph")
        readback = self._make_readback(graphs=[{"name": "EventGraph", "node_count": 1}], is_compiled=True)
        result = verify_blueprint(spec, readback)
        assert result.verified is False
        assert result.checks["graph_exists"].passed is False

    def test_node_count_mismatch(self):
        spec = self._make_spec(nodes=[{"name": "A", "class": "Event"}, {"name": "B", "class": "CallFunction"}])
        readback = self._make_readback(
            is_compiled=True,
            graphs=[{"name": "EventGraph", "node_count": 1}],
            nodes=[{"class": "K2Node_Event"}],
        )
        result = verify_blueprint(spec, readback)
        assert result.verified is False
        assert result.checks["graph_node_count"].passed is False

    def test_no_connections_check_present(self):
        spec = self._make_spec(nodes=[{"name": "A", "class": "Event"}])
        readback = self._make_readback(is_compiled=True, graphs=[{"name": "EventGraph", "node_count": 1}])
        result = verify_blueprint(spec, readback)
        assert "connection_count" not in result.checks

    def test_duplicate_spec_class_requires_matching_count(self):
        spec = self._make_spec(
            nodes=[
                {"name": "A", "class": "CallFunction"},
                {"name": "B", "class": "CallFunction"},
            ]
        )
        readback = self._make_readback(
            is_compiled=True,
            graphs=[{"name": "EventGraph", "node_count": 3}],
            nodes=[{"class": "K2Node_CallFunction"}],
        )
        result = verify_blueprint(spec, readback)
        assert result.verified is False
        assert result.checks["node_count:CallFunction"].passed is False


class TestUMGVerification:
    def _make_spec(self, widgets=None):
        return {"widgets": widgets or []}

    def _make_readback(self, tree=None):
        return {"tree": tree or {}}

    def test_all_checks_pass(self):
        spec = self._make_spec(
            widgets=[
                {
                    "name": "Root",
                    "class": "CanvasPanel",
                    "children": [
                        {"name": "Title", "class": "TextBlock"},
                        {"name": "StartButton", "class": "Button"},
                    ],
                }
            ]
        )
        readback = self._make_readback(
            tree={
                "name": "Root",
                "class": "CanvasPanel",
                "children": [
                    {"name": "Title", "class": "TextBlock", "children": []},
                    {"name": "StartButton", "class": "Button", "children": []},
                ],
            }
        )
        result = verify_umg(spec, readback)
        assert result.verified is True

    def test_missing_widget(self):
        spec = self._make_spec(
            widgets=[
                {
                    "name": "Root",
                    "class": "CanvasPanel",
                    "children": [{"name": "Title", "class": "TextBlock"}],
                }
            ]
        )
        readback = self._make_readback(tree={"name": "Root", "class": "CanvasPanel", "children": []})
        result = verify_umg(spec, readback)
        assert result.verified is False
        assert result.checks["widget_exists:Title"].passed is False

    def test_widget_class_mismatch(self):
        spec = self._make_spec(widgets=[{"name": "Root", "class": "CanvasPanel"}])
        readback = self._make_readback(tree={"name": "Root", "class": "Overlay", "children": []})
        result = verify_umg(spec, readback)
        assert result.verified is False
        assert result.checks["widget_class:Root"].passed is False

    def test_widget_count_mismatch(self):
        spec = self._make_spec(
            widgets=[
                {
                    "name": "Root",
                    "class": "CanvasPanel",
                    "children": [
                        {"name": "Title", "class": "TextBlock"},
                        {"name": "Body", "class": "TextBlock"},
                    ],
                }
            ]
        )
        readback = self._make_readback(
            tree={
                "name": "Root",
                "class": "CanvasPanel",
                "children": [{"name": "Title", "class": "TextBlock", "children": []}],
            }
        )
        result = verify_umg(spec, readback)
        assert result.verified is False
        assert result.checks["widget_count"].passed is False

    def test_empty_tree_detected(self):
        spec = self._make_spec(widgets=[{"name": "Root", "class": "CanvasPanel"}])
        readback = self._make_readback(tree={})
        result = verify_umg(spec, readback)
        assert result.verified is False
        assert result.checks["widget_count"].passed is False

    def test_serialization_contains_expected_fields(self):
        spec = self._make_spec(widgets=[{"name": "Root", "class": "CanvasPanel"}])
        readback = self._make_readback(tree={"name": "Root", "class": "CanvasPanel", "children": []})
        result = verify_umg(spec, readback)
        payload = result.to_dict()
        assert "verified" in payload
        assert "checks" in payload
