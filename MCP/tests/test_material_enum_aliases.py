"""Unit tests for material property enum alias normalization."""

import sys
from pathlib import Path

# Add tools directory to path for imports
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from material.assets import _normalize_enum_value


class TestEnumAliasNormalization:
    """Test _normalize_enum_value() function."""

    def test_pretty_name_to_reflection_blend_mode(self):
        """Pretty name maps to reflection name for BlendMode."""
        assert _normalize_enum_value("BlendMode", "Opaque") == "BLEND_Opaque"
        assert _normalize_enum_value("BlendMode", "Masked") == "BLEND_Masked"
        assert _normalize_enum_value("BlendMode", "Translucent") == "BLEND_Translucent"

    def test_pretty_name_to_reflection_material_domain(self):
        """Pretty name maps to reflection name for MaterialDomain."""
        assert _normalize_enum_value("MaterialDomain", "Surface") == "MD_Surface"
        assert _normalize_enum_value("MaterialDomain", "PostProcess") == "MD_PostProcess"
        assert _normalize_enum_value("MaterialDomain", "UI") == "MD_UI"

    def test_pretty_name_to_reflection_shading_model(self):
        """Pretty name maps to reflection name for ShadingModel."""
        assert _normalize_enum_value("ShadingModel", "Unlit") == "MSM_Unlit"
        assert _normalize_enum_value("ShadingModel", "DefaultLit") == "MSM_DefaultLit"

    def test_reflection_name_passthrough(self):
        """Reflection names pass through unchanged."""
        assert _normalize_enum_value("BlendMode", "BLEND_Opaque") == "BLEND_Opaque"
        assert _normalize_enum_value("MaterialDomain", "MD_PostProcess") == "MD_PostProcess"
        assert _normalize_enum_value("ShadingModel", "MSM_Unlit") == "MSM_Unlit"

    def test_unknown_property_passthrough(self):
        """Unknown property names pass value through unchanged."""
        assert _normalize_enum_value("TwoSided", "true") == "true"
        assert _normalize_enum_value("UnknownProp", "SomeValue") == "SomeValue"

    def test_unknown_value_passthrough(self):
        """Unknown values for known properties pass through unchanged."""
        assert _normalize_enum_value("BlendMode", "CustomBlend") == "CustomBlend"
