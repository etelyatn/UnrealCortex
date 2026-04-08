"""Tests for level query Blueprint naming hints."""

from tools.level.queries import _has_blueprint_actors, _inject_blueprint_hint, _BLUEPRINT_HINT


class TestHasBlueprintActors:
    def test_blueprint_class_detected_in_actors(self):
        data = {"actors": [{"name": "BP_Lift_C_1", "label": "BP_Lift", "class": "BP_Lift_C"}]}
        assert _has_blueprint_actors(data) is True

    def test_blueprint_class_detected_in_matches(self):
        data = {"matches": [{"name": "BP_Lift_C_1", "label": "BP_Lift", "class": "BP_Lift_C"}]}
        assert _has_blueprint_actors(data) is True

    def test_native_class_not_detected(self):
        data = {"actors": [{"name": "PointLight_0", "label": "MyLight", "class": "PointLight"}]}
        assert _has_blueprint_actors(data) is False

    def test_empty_actors(self):
        data = {"actors": [], "count": 0}
        assert _has_blueprint_actors(data) is False

    def test_no_actors_key(self):
        data = {"count": 0}
        assert _has_blueprint_actors(data) is False

    def test_mixed_native_and_blueprint(self):
        data = {
            "actors": [
                {"name": "PointLight_0", "label": "MyLight", "class": "PointLight"},
                {"name": "BP_Door_C_1", "label": "BP_Door", "class": "BP_Door_C"},
            ]
        }
        assert _has_blueprint_actors(data) is True

    def test_blueprint_starting_with_a(self):
        """Blueprint named ABP_Character should still be detected."""
        data = {"actors": [{"name": "ABP_Character_C_1", "class": "ABP_Character_C"}]}
        assert _has_blueprint_actors(data) is True


class TestInjectBlueprintHint:
    def test_hint_injected_when_blueprint_present(self):
        data = {"matches": [{"class": "BP_Lift_C"}]}
        _inject_blueprint_hint(data)
        assert data.get("hint") == _BLUEPRINT_HINT

    def test_no_hint_when_only_native(self):
        data = {"actors": [{"class": "PointLight"}]}
        _inject_blueprint_hint(data)
        assert "hint" not in data

    def test_no_hint_on_empty(self):
        data = {"actors": []}
        _inject_blueprint_hint(data)
        assert "hint" not in data
