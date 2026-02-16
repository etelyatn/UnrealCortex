"""Unit tests for UMG widget composite tool helper functions."""

import sys
from pathlib import Path

import pytest

tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from umg.composites import (
    _flatten_widget_tree,
    _validate_widget_spec,
    _contains_ref_syntax,
)


class TestTreeFlattening:
    """Test _flatten_widget_tree() function."""

    def test_single_widget(self):
        """Single widget with no children."""
        widgets = [{"class": "CanvasPanel", "name": "Root"}]
        result = _flatten_widget_tree(widgets)
        assert len(result) == 1
        assert result[0] == (widgets[0], "")

    def test_parent_child(self):
        """Parent before children, parent_name set correctly."""
        widgets = [{
            "class": "CanvasPanel", "name": "Root",
            "children": [{"class": "TextBlock", "name": "Title"}],
        }]
        result = _flatten_widget_tree(widgets)
        assert len(result) == 2
        assert result[0] == (widgets[0], "")
        assert result[1][0]["name"] == "Title"
        assert result[1][1] == "Root"

    def test_depth_first_order(self):
        """Depth-first traversal: parent, then all descendants before siblings."""
        widgets = [{
            "class": "CanvasPanel", "name": "Root",
            "children": [
                {"class": "VerticalBox", "name": "Col1",
                 "children": [{"class": "TextBlock", "name": "Text1"}]},
                {"class": "VerticalBox", "name": "Col2"},
            ],
        }]
        result = _flatten_widget_tree(widgets)
        names = [w[0]["name"] for w in result]
        assert names == ["Root", "Col1", "Text1", "Col2"]

    def test_deeply_nested_hierarchy(self):
        """4+ levels of nesting."""
        widgets = [{
            "class": "CanvasPanel", "name": "L0",
            "children": [{
                "class": "Overlay", "name": "L1",
                "children": [{
                    "class": "VerticalBox", "name": "L2",
                    "children": [{
                        "class": "HorizontalBox", "name": "L3",
                        "children": [{"class": "TextBlock", "name": "L4"}],
                    }],
                }],
            }],
        }]
        result = _flatten_widget_tree(widgets)
        names = [w[0]["name"] for w in result]
        assert names == ["L0", "L1", "L2", "L3", "L4"]
        # Verify parent chain
        parents = [w[1] for w in result]
        assert parents == ["", "L0", "L1", "L2", "L3"]

    def test_multiple_roots(self):
        """Multiple root widgets flatten correctly."""
        widgets = [
            {"class": "TextBlock", "name": "A"},
            {"class": "TextBlock", "name": "B"},
        ]
        result = _flatten_widget_tree(widgets)
        assert len(result) == 2
        assert result[0][1] == ""
        assert result[1][1] == ""


class TestWidgetValidation:
    """Test _validate_widget_spec() function."""

    def test_missing_name(self):
        with pytest.raises(ValueError, match="Missing required field: name"):
            _validate_widget_spec("", "/Game/UI/", [{"class": "CanvasPanel", "name": "R"}])

    def test_missing_path(self):
        with pytest.raises(ValueError, match="Missing required field: path"):
            _validate_widget_spec("WBP_Test", "", [{"class": "CanvasPanel", "name": "R"}])

    def test_empty_widgets(self):
        with pytest.raises(ValueError, match="widgets must not be empty"):
            _validate_widget_spec("WBP_Test", "/Game/UI/", [])

    def test_widget_missing_class(self):
        with pytest.raises(ValueError, match="missing 'class'"):
            _validate_widget_spec("WBP_Test", "/Game/UI/", [{"name": "R"}])

    def test_widget_missing_name(self):
        with pytest.raises(ValueError, match="missing 'name'"):
            _validate_widget_spec("WBP_Test", "/Game/UI/", [{"class": "CanvasPanel"}])

    def test_duplicate_names_across_hierarchy(self):
        widgets = [{
            "class": "CanvasPanel", "name": "Root",
            "children": [{"class": "TextBlock", "name": "Root"}],
        }]
        with pytest.raises(ValueError, match="Duplicate widget names"):
            _validate_widget_spec("WBP_Test", "/Game/UI/", widgets)

    def test_duplicate_animation_names(self):
        widgets = [{"class": "CanvasPanel", "name": "Root"}]
        animations = [{"name": "FadeIn"}, {"name": "FadeIn"}]
        with pytest.raises(ValueError, match="Duplicate animation names"):
            _validate_widget_spec("WBP_Test", "/Game/UI/", widgets, animations)

    def test_ref_syntax_in_widget_values_rejected(self):
        widgets = [{"class": "CanvasPanel", "name": "Root", "text": "$steps[0].data.x"}]
        with pytest.raises(ValueError, match=r"\$steps\["):
            _validate_widget_spec("WBP_Test", "/Game/UI/", widgets)

    def test_valid_full_spec(self):
        """Full valid spec should pass."""
        widgets = [{
            "class": "CanvasPanel", "name": "Root",
            "children": [
                {"class": "TextBlock", "name": "Title", "text": "Hello"},
                {"class": "Button", "name": "Btn"},
            ],
        }]
        _validate_widget_spec("WBP_Test", "/Game/UI/", widgets, [{"name": "FadeIn", "length": 1.0}])
