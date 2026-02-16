"""MCP composite tools for high-level Widget Blueprint screen creation."""

import json
import logging
from typing import Any
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)

# Styling shorthand keys that map to specific UMG commands
_STYLING_KEYS = {
    "text": "umg.set_text",
    "color": "umg.set_color",
    "font": "umg.set_font",
    "brush": "umg.set_brush",
    "padding": "umg.set_padding",
    "anchor": "umg.set_anchor",
    "alignment": "umg.set_alignment",
    "size": "umg.set_size",
    "visibility": "umg.set_visibility",
}

# Keys that are structural, not styling
_STRUCTURAL_KEYS = {"class", "name", "children", "properties"}


def _contains_ref_syntax(value):
    """Check if value or any nested element contains $steps[ syntax."""
    if isinstance(value, str):
        return value.startswith("$steps[")
    elif isinstance(value, list):
        return any(_contains_ref_syntax(v) for v in value)
    elif isinstance(value, dict):
        return any(_contains_ref_syntax(v) for v in value.values())
    return False


def _flatten_widget_tree(widgets, parent_name=""):
    """Flatten hierarchical widget tree to depth-first list of (widget, parent_name)."""
    result = []
    for widget in widgets:
        result.append((widget, parent_name))
        result.extend(_flatten_widget_tree(widget.get("children", []), widget["name"]))
    return result


def _collect_widget_names(widgets):
    """Recursively collect all widget names for uniqueness check."""
    names = []
    for widget in widgets:
        if "name" in widget:
            names.append(widget["name"])
        names.extend(_collect_widget_names(widget.get("children", [])))
    return names


def _validate_widget_spec(
    name: str,
    path: str,
    widgets: list[dict],
    animations: list[dict] | None = None,
):
    """Validate widget screen spec. Raises ValueError on invalid spec."""
    if not name:
        raise ValueError("Missing required field: name")
    if not path:
        raise ValueError("Missing required field: path")
    if not widgets:
        raise ValueError("widgets must not be empty")

    animations = animations or []

    # Validate each widget has class and name
    def _validate_widgets_recursive(widget_list, depth=0):
        for i, w in enumerate(widget_list):
            if "class" not in w:
                raise ValueError(f"Widget at depth {depth} index {i} missing 'class' field")
            if "name" not in w:
                raise ValueError(f"Widget at depth {depth} index {i} missing 'name' field")
            # Check for $steps[ in styling values
            for key, value in w.items():
                if key in _STRUCTURAL_KEYS:
                    continue
                if _contains_ref_syntax(value):
                    raise ValueError(
                        f"Widget '{w.get('name')}' key '{key}' contains '$steps[' "
                        f"which conflicts with batch $ref syntax"
                    )
            # Check properties dict too
            for key, value in (w.get("properties") or {}).items():
                if _contains_ref_syntax(value):
                    raise ValueError(
                        f"Widget '{w.get('name')}' property '{key}' contains '$steps[' "
                        f"which conflicts with batch $ref syntax"
                    )
            _validate_widgets_recursive(w.get("children", []), depth + 1)

    _validate_widgets_recursive(widgets)

    # Widget name uniqueness (recursive)
    all_names = _collect_widget_names(widgets)
    if len(all_names) != len(set(all_names)):
        raise ValueError("Duplicate widget names in spec")

    # Animation name uniqueness
    anim_names = [a.get("name", "") for a in animations]
    if len(anim_names) != len(set(anim_names)):
        raise ValueError("Duplicate animation names in spec")
