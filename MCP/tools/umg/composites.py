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


def _build_widget_batch_commands(
    name: str,
    path: str,
    widgets: list[dict],
    animations: list[dict],
) -> list[dict]:
    """Translate widget screen spec into batch commands."""
    path = path.rstrip("/")
    commands = []

    # Step 0: create Widget Blueprint
    commands.append({
        "command": "bp.create",
        "params": {"name": name, "path": path, "type": "Widget"},
    })

    # Flatten widget tree (depth-first, parent before children)
    flat_widgets = _flatten_widget_tree(widgets)

    # Add widget commands (all widgets before any styling)
    for widget, parent_name in flat_widgets:
        commands.append({
            "command": "umg.add_widget",
            "params": {
                "asset_path": "$steps[0].data.asset_path",
                "widget_class": widget["class"],
                "widget_name": widget["name"],
                "parent_name": parent_name,
            },
        })

    # Styling commands (after all widgets exist)
    for widget, _parent in flat_widgets:
        widget_name = widget["name"]

        for key, value in widget.items():
            if key in _STRUCTURAL_KEYS:
                continue

            if key in _STYLING_KEYS:
                command_name = _STYLING_KEYS[key]
                params = {"asset_path": "$steps[0].data.asset_path", "widget_name": widget_name}

                if key == "text":
                    params["text"] = value
                elif key == "color":
                    params["color"] = value
                    params["target"] = "foreground"
                elif key == "font":
                    if isinstance(value, dict):
                        params.update(value)
                    else:
                        params["size"] = value
                elif key == "brush":
                    if isinstance(value, dict):
                        params.update(value)
                elif key == "padding":
                    if isinstance(value, dict):
                        params.update(value)
                    else:
                        params["padding"] = value
                elif key == "anchor":
                    params["preset"] = value
                elif key == "alignment":
                    if isinstance(value, dict):
                        params.update(value)
                elif key == "size":
                    if isinstance(value, dict):
                        params.update(value)
                elif key == "visibility":
                    params["visibility"] = value

                commands.append({"command": command_name, "params": params})

        # Generic properties
        for prop_path, prop_value in (widget.get("properties") or {}).items():
            commands.append({
                "command": "umg.set_property",
                "params": {
                    "asset_path": "$steps[0].data.asset_path",
                    "widget_name": widget_name,
                    "property_path": prop_path,
                    "value": prop_value,
                },
            })

    # Animation commands
    for anim in animations:
        anim_params = {
            "asset_path": "$steps[0].data.asset_path",
            "animation_name": anim["name"],
        }
        if "length" in anim:
            anim_params["length"] = anim["length"]
        commands.append({"command": "umg.create_animation", "params": anim_params})

    return commands
