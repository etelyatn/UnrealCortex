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


def register_umg_composite_tools(mcp, connection: UEConnection):
    """Register UMG widget composite MCP tools."""

    @mcp.tool()
    def create_widget_screen(
        name: str,
        path: str,
        widgets: list[dict],
        animations: list[dict] = None,
    ) -> str:
        """Create a Widget Blueprint screen with full widget hierarchy and styling in a single operation.

        Creates a Widget Blueprint, builds the widget tree with proper nesting, applies all
        styling (text, colors, fonts, brushes, anchors, etc.), and adds animations.
        All operations execute atomically via batch.

        Use this instead of calling create_blueprint + add_widget + set_text + set_color
        individually when building a complete UI screen from scratch.

        Args:
            name: Widget Blueprint name (e.g., "WBP_MainMenu")
            path: Directory path (e.g., "/Game/UI/")
            widgets: Hierarchical widget tree. Each widget has:
                - class: Widget class name (CanvasPanel, VerticalBox, HorizontalBox,
                    Overlay, TextBlock, Button, Image, ScrollBox, etc.)
                - name: Unique identifier
                - children: Optional nested child widgets
                - text: Set text content (TextBlock/RichTextBlock)
                - color: Set foreground color (hex string like "#FFFFFF")
                - font: Set font (dict with size, typeface, letter_spacing)
                - brush: Set brush (dict with target, color, draw_as, corner_radius)
                - padding: Set padding (number for uniform, or dict {left,top,right,bottom})
                - anchor: Set anchor preset (string like "FullStretch", "Center")
                - alignment: Set alignment (dict with horizontal, vertical)
                - size: Set size (dict with width, height, size_rule)
                - visibility: Set visibility ("Visible", "Collapsed", "Hidden")
                - properties: Dict of generic property_path:value pairs for anything else
            animations: Optional array of animation specs:
                - name: Animation name
                - length: Duration in seconds

        Returns:
            JSON with asset_path, widget_count, styling_count, animation_count, timing.

        Example:
            create_widget_screen(
                name="WBP_MainMenu", path="/Game/UI/",
                widgets=[{
                    "class": "CanvasPanel", "name": "Root",
                    "children": [{
                        "class": "VerticalBox", "name": "Layout", "anchor": "FullStretch",
                        "children": [
                            {"class": "TextBlock", "name": "Title", "text": "Main Menu",
                             "font": {"size": 48, "typeface": "Bold"}, "color": "#FFFFFF"},
                            {"class": "Button", "name": "PlayBtn",
                             "brush": {"target": "normal", "color": "#2196F3"}},
                        ]
                    }]
                }],
                animations=[{"name": "FadeIn", "length": 0.5}]
            )
        """
        animations = animations or []

        # 1. Validate spec
        try:
            _validate_widget_spec(name, path, widgets, animations)
        except ValueError as e:
            return json.dumps({"success": False, "error": f"Invalid spec: {e}"})

        # 2. Build batch commands
        commands = _build_widget_batch_commands(name, path, widgets, animations)
        total_steps = len(commands)

        # 3. Send batch
        timeout = max(60, len(commands) * 2)
        try:
            batch_result = connection.send_command("batch", {
                "stop_on_error": True,
                "commands": commands,
            }, timeout=timeout)
        except RuntimeError as e:
            return json.dumps({"success": False, "error": str(e)})
        except (ConnectionError, TimeoutError, OSError) as e:
            return json.dumps({"success": False, "error": f"Connection error: {e}"})

        batch_data = batch_result.get("data", {})
        results = batch_data.get("results", [])

        # 4. Check for failures
        asset_path = None
        failed_step = None
        completed_count = 0

        for entry in results:
            if entry.get("success"):
                completed_count += 1
                if entry.get("index") == 0 and "data" in entry:
                    asset_path = entry["data"].get("asset_path")
            else:
                failed_step = entry
                break

        # 5. Handle failure
        if failed_step is not None:
            recovery_action = None
            if asset_path:
                try:
                    connection.send_command("bp.delete", {"asset_path": asset_path, "force": True})
                    recovery_action = {"action": "deleted_partial", "path": asset_path}
                except Exception as e:
                    recovery_action = {
                        "action": "cleanup_failed",
                        "path": asset_path,
                        "error": str(e),
                        "user_action_required": f"Manually delete partial asset at {asset_path}",
                    }

            response = {
                "success": False,
                "summary": f"Step {failed_step['index']} of {total_steps} failed: "
                           f"{failed_step.get('command', '?')} - "
                           f"{failed_step.get('error_message', 'Unknown error')}",
                "asset_path": asset_path,
                "completed_steps": completed_count,
                "failed_step": {
                    "index": failed_step["index"],
                    "command": failed_step.get("command", ""),
                    "error": failed_step.get("error_message", ""),
                },
                "total_steps": total_steps,
            }
            if recovery_action:
                response["recovery_action"] = recovery_action
            return json.dumps(response, indent=2)

        # 6. Success — post-batch
        warnings = []
        if asset_path:
            try:
                connection.send_command("bp.compile", {"asset_path": asset_path})
            except Exception as e:
                logger.warning(f"compile failed for {asset_path}: {e}", exc_info=True)
                warnings.append({"step": "compile", "error": str(e)})

            try:
                connection.send_command("bp.save", {"asset_path": asset_path})
            except Exception as e:
                logger.warning(f"save failed for {asset_path}: {e}", exc_info=True)
                warnings.append({"step": "save", "error": str(e)})

        widget_count = sum(1 for c in commands if c["command"] == "umg.add_widget")
        styling_count = sum(1 for c in commands if c["command"].startswith("umg.set_"))
        anim_count = sum(1 for c in commands if c["command"] == "umg.create_animation")
        prop_count = sum(1 for c in commands if c["command"] == "umg.set_property")

        response = {
            "success": True,
            "asset_path": asset_path,
            "total_steps": total_steps,
            "widget_count": widget_count,
            "styling_count": styling_count,
            "property_count": prop_count,
            "animation_count": anim_count,
            "total_timing_ms": batch_data.get("total_timing_ms", 0),
        }
        if warnings:
            response["warnings"] = warnings

        return json.dumps(response, indent=2)
