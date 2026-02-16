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
    _build_widget_batch_commands,
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


class TestBatchCommandGeneration:
    """Test _build_widget_batch_commands() function."""

    def test_single_root_widget(self):
        """Single root widget generates create + add_widget commands."""
        widgets = [{"class": "CanvasPanel", "name": "Root"}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])

        assert commands[0]["command"] == "bp.create"
        assert commands[0]["params"]["type"] == "Widget"
        assert commands[1]["command"] == "umg.add_widget"
        assert commands[1]["params"]["widget_class"] == "CanvasPanel"
        assert commands[1]["params"]["widget_name"] == "Root"
        assert commands[1]["params"]["parent_name"] == ""

    def test_nested_widgets_depth_first(self):
        """Nested widgets create add_widget in depth-first order with correct parent."""
        widgets = [{
            "class": "CanvasPanel", "name": "Root",
            "children": [
                {"class": "TextBlock", "name": "Title"},
                {"class": "Button", "name": "Btn"},
            ],
        }]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])

        add_cmds = [c for c in commands if c["command"] == "umg.add_widget"]
        assert len(add_cmds) == 3
        assert add_cmds[0]["params"]["widget_name"] == "Root"
        assert add_cmds[0]["params"]["parent_name"] == ""
        assert add_cmds[1]["params"]["widget_name"] == "Title"
        assert add_cmds[1]["params"]["parent_name"] == "Root"
        assert add_cmds[2]["params"]["widget_name"] == "Btn"
        assert add_cmds[2]["params"]["parent_name"] == "Root"

    def test_styling_commands_after_all_widgets(self):
        """Styling commands come after all add_widget commands."""
        widgets = [{
            "class": "CanvasPanel", "name": "Root",
            "children": [
                {"class": "TextBlock", "name": "Title", "text": "Hello", "color": "#FFFFFF"},
            ],
        }]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])

        cmd_types = [c["command"] for c in commands]
        # All add_widget before any styling
        last_add_idx = max(i for i, t in enumerate(cmd_types) if t == "umg.add_widget")
        styling_idxs = [i for i, t in enumerate(cmd_types) if t.startswith("umg.set_")]
        for idx in styling_idxs:
            assert idx > last_add_idx

    def test_text_styling_command(self):
        """text shorthand generates umg.set_text command."""
        widgets = [{"class": "TextBlock", "name": "Title", "text": "Hello World"}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])

        text_cmds = [c for c in commands if c["command"] == "umg.set_text"]
        assert len(text_cmds) == 1
        assert text_cmds[0]["params"]["widget_name"] == "Title"
        assert text_cmds[0]["params"]["text"] == "Hello World"

    def test_font_styling_command(self):
        """font shorthand generates umg.set_font command."""
        widgets = [{"class": "TextBlock", "name": "T", "font": {"size": 48, "typeface": "Bold"}}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])

        font_cmds = [c for c in commands if c["command"] == "umg.set_font"]
        assert len(font_cmds) == 1
        assert font_cmds[0]["params"]["size"] == 48
        assert font_cmds[0]["params"]["typeface"] == "Bold"

    def test_brush_styling_command(self):
        """brush shorthand generates umg.set_brush command."""
        widgets = [{"class": "Button", "name": "Btn",
                     "brush": {"target": "normal", "color": "#2196F3", "draw_as": "RoundedBox", "corner_radius": 12}}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])

        brush_cmds = [c for c in commands if c["command"] == "umg.set_brush"]
        assert len(brush_cmds) == 1
        assert brush_cmds[0]["params"]["target"] == "normal"
        assert brush_cmds[0]["params"]["color"] == "#2196F3"

    def test_padding_dict_styling(self):
        """padding dict shorthand generates umg.set_padding with all sides."""
        widgets = [{"class": "VerticalBox", "name": "VB", "padding": {"left": 10, "top": 20, "right": 10, "bottom": 20}}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])

        pad_cmds = [c for c in commands if c["command"] == "umg.set_padding"]
        assert len(pad_cmds) == 1
        assert pad_cmds[0]["params"]["left"] == 10
        assert pad_cmds[0]["params"]["top"] == 20

    def test_padding_number_styling(self):
        """padding number shorthand generates umg.set_padding with uniform value."""
        widgets = [{"class": "VerticalBox", "name": "VB", "padding": 16}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])

        pad_cmds = [c for c in commands if c["command"] == "umg.set_padding"]
        assert len(pad_cmds) == 1
        assert pad_cmds[0]["params"]["padding"] == 16

    def test_properties_generate_set_property_commands(self):
        """properties dict generates umg.set_property for each key."""
        widgets = [{"class": "Image", "name": "Img",
                     "properties": {"Brush.ImageSize.X": 64, "Brush.ImageSize.Y": 64}}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])

        prop_cmds = [c for c in commands if c["command"] == "umg.set_property"]
        assert len(prop_cmds) == 2

    def test_animations_after_styling(self):
        """Animation commands come after all styling."""
        widgets = [{"class": "CanvasPanel", "name": "Root", "text": "X"}]
        animations = [{"name": "FadeIn", "length": 1.0}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, animations)

        anim_cmds = [c for c in commands if c["command"] == "umg.create_animation"]
        assert len(anim_cmds) == 1
        assert anim_cmds[0]["params"]["animation_name"] == "FadeIn"
        assert anim_cmds[0]["params"]["length"] == 1.0

    def test_asset_path_uses_ref(self):
        """All commands after step 0 use $steps[0].data.asset_path."""
        widgets = [{"class": "CanvasPanel", "name": "Root"}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])

        for cmd in commands[1:]:
            assert cmd["params"]["asset_path"] == "$steps[0].data.asset_path"

    def test_trailing_slash_normalized(self):
        """Trailing slash on path is normalized."""
        widgets = [{"class": "CanvasPanel", "name": "Root"}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])
        assert commands[0]["params"]["path"] == "/Game/UI"


class TestCleanupOnFailure:
    def test_batch_step_0_is_create(self):
        """Step 0 must be bp.create for cleanup to work."""
        widgets = [{"class": "CanvasPanel", "name": "Root"}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])
        assert commands[0]["command"] == "bp.create"


class TestTimeoutScaling:
    def test_small_screen_uses_minimum(self):
        widgets = [{"class": "CanvasPanel", "name": "Root"}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])
        expected = max(60, len(commands) * 2)
        assert expected == 60

    def test_large_screen_scales(self):
        children = [{"class": "TextBlock", "name": f"T{i}", "text": f"Item {i}"} for i in range(40)]
        widgets = [{"class": "VerticalBox", "name": "Root", "children": children}]
        commands = _build_widget_batch_commands("WBP_Test", "/Game/UI/", widgets, [])
        expected = max(60, len(commands) * 2)
        assert expected > 60
