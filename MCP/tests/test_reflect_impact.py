"""Unit tests for impact analysis helper functions."""

import sys
from pathlib import Path

# Add tools directory to path
TOOLS_DIR = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from reflect.impact import _build_recommendation, _score_risk  # noqa: E402


class TestScoreRisk:
    """Test _score_risk() risk scoring logic."""

    def test_function_call_is_high_risk(self):
        risk, reason = _score_risk([{"type": "call"}], "TakeDamage")
        assert risk == "high"
        assert "compile" in reason.lower()

    def test_property_read_is_high_risk(self):
        risk, _ = _score_risk([{"type": "read"}], "Health")
        assert risk == "high"

    def test_property_write_is_high_risk(self):
        risk, _ = _score_risk([{"type": "write"}], "Health")
        assert risk == "high"

    def test_override_is_high_risk(self):
        risk, _ = _score_risk([{"type": "override"}], "TakeDamage")
        assert risk == "high"

    def test_dynamic_cast_is_medium_risk(self):
        risk, reason = _score_risk([{"type": "cast"}], "AMyCharacter")
        assert risk == "medium"
        assert "null" in reason.lower() or "runtime" in reason.lower()

    def test_spawn_is_medium_risk(self):
        risk, _ = _score_risk([{"type": "spawn"}], "AMyActor")
        assert risk == "medium"

    def test_component_is_medium_risk(self):
        risk, _ = _score_risk([{"type": "component"}], "UHealthComponent")
        assert risk == "medium"

    def test_empty_usages_is_low_risk(self):
        risk, reason = _score_risk([], "Anything")
        assert risk == "low"
        assert "indirect" in reason.lower()

    def test_worst_risk_wins(self):
        risk, _ = _score_risk([{"type": "cast"}, {"type": "call"}], "TakeDamage")
        assert risk == "high"

    def test_unknown_type_treated_as_low(self):
        risk, _ = _score_risk([{"type": "unknown_type"}], "X")
        assert risk == "low"

    def test_missing_type_key_treated_as_low(self):
        risk, _ = _score_risk([{"context": "EventGraph"}], "X")
        assert risk == "low"

    def test_mixed_unknown_and_known(self):
        risk, _ = _score_risk([{"type": "unknown"}, {"type": "cast"}], "X")
        assert risk == "medium"

    def test_removed_class_all_high_risk(self):
        risk, _ = _score_risk([{"type": "cast"}], "AMyClass", change_type="removed_class")
        assert risk == "high"

    def test_removed_class_empty_usages_is_low(self):
        risk, _ = _score_risk([], "AMyClass", change_type="removed_class")
        assert risk == "low"

    def test_changed_hierarchy_elevates_cast(self):
        risk, _ = _score_risk([{"type": "cast"}], "AMyClass", change_type="changed_hierarchy")
        assert risk == "high"

    def test_changed_hierarchy_does_not_affect_call(self):
        risk, _ = _score_risk([{"type": "call"}], "Func", change_type="changed_hierarchy")
        assert risk == "high"


class TestBuildRecommendation:
    """Test _build_recommendation() output."""

    def test_no_affected(self):
        rec = _build_recommendation(0, {"high": 0, "medium": 0, "low": 0}, "Foo")
        assert "No Blueprints affected" in rec

    def test_high_risk_mentioned(self):
        rec = _build_recommendation(3, {"high": 2, "medium": 1, "low": 0}, "TakeDamage")
        assert "3 Blueprints" in rec
        assert "2 high-risk" in rec

    def test_medium_risk_mentioned(self):
        rec = _build_recommendation(1, {"high": 0, "medium": 1, "low": 0}, "Foo")
        assert "1 medium-risk" in rec

    def test_single_blueprint_grammar(self):
        rec = _build_recommendation(1, {"high": 1, "medium": 0, "low": 0}, "X")
        assert "1 Blueprint affected" in rec

    def test_low_risk_only(self):
        rec = _build_recommendation(5, {"high": 0, "medium": 0, "low": 5}, "Foo")
        assert "5 Blueprints" in rec
        assert "high-risk" not in rec
        assert "medium-risk" not in rec

    def test_all_risk_levels(self):
        rec = _build_recommendation(6, {"high": 2, "medium": 3, "low": 1}, "Bar")
        assert "2 high-risk" in rec
        assert "3 medium-risk" in rec
