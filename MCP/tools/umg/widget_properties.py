"""MCP tools for UMG widget property operations."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_SCHEMA = 1800


def register_widget_property_tools(mcp, connection: UEConnection):
    """Register all widget property MCP tools."""

    @mcp.tool()
    def set_color(
        asset_path: str,
        widget_name: str,
        color: str,
        target: str = "foreground",
    ) -> str:
        """Set a color on a widget."""
        try:
            response = connection.send_command("umg.set_color", {
                "asset_path": asset_path,
                "widget_name": widget_name,
                "color": color,
                "target": target,
            })
            return format_response(response.get("data", {}), "set_color")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_text(asset_path: str, widget_name: str, text: str) -> str:
        """Set text content on text-capable widgets."""
        try:
            response = connection.send_command("umg.set_text", {
                "asset_path": asset_path,
                "widget_name": widget_name,
                "text": text,
            })
            return format_response(response.get("data", {}), "set_text")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_font(
        asset_path: str, widget_name: str, family: str = "", size: int = 0,
        typeface: str = "", letter_spacing: float = 0,
    ) -> str:
        """Set font properties on a text widget."""
        params = {"asset_path": asset_path, "widget_name": widget_name}
        if family:
            params["family"] = family
        if size > 0:
            params["size"] = size
        if typeface:
            params["typeface"] = typeface
        if letter_spacing != 0:
            params["letter_spacing"] = letter_spacing
        try:
            response = connection.send_command("umg.set_font", params)
            return format_response(response.get("data", {}), "set_font")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_brush(
        asset_path: str, widget_name: str, target: str,
        color: str = "", draw_as: str = "", corner_radius: float = 0,
    ) -> str:
        """Set brush appearance for widget states."""
        params = {"asset_path": asset_path, "widget_name": widget_name, "target": target}
        if color:
            params["color"] = color
        if draw_as:
            params["draw_as"] = draw_as
        if corner_radius > 0:
            params["corner_radius"] = corner_radius
        try:
            response = connection.send_command("umg.set_brush", params)
            return format_response(response.get("data", {}), "set_brush")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_padding(asset_path: str, widget_name: str, padding: str, target: str = "padding") -> str:
        """Set padding or margin."""
        params = {"asset_path": asset_path, "widget_name": widget_name, "target": target}
        try:
            params["padding"] = json.loads(padding)
        except (json.JSONDecodeError, TypeError):
            params["padding"] = float(padding)
        try:
            response = connection.send_command("umg.set_padding", params)
            return format_response(response.get("data", {}), "set_padding")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_anchor(asset_path: str, widget_name: str, preset: str = "") -> str:
        """Set anchor on a CanvasPanel child."""
        params = {"asset_path": asset_path, "widget_name": widget_name}
        if preset:
            params["preset"] = preset
        try:
            response = connection.send_command("umg.set_anchor", params)
            return format_response(response.get("data", {}), "set_anchor")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_alignment(
        asset_path: str, widget_name: str, horizontal: str = "", vertical: str = "",
    ) -> str:
        """Set horizontal/vertical alignment within a slot."""
        params = {"asset_path": asset_path, "widget_name": widget_name}
        if horizontal:
            params["horizontal"] = horizontal
        if vertical:
            params["vertical"] = vertical
        try:
            response = connection.send_command("umg.set_alignment", params)
            return format_response(response.get("data", {}), "set_alignment")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_size(
        asset_path: str, widget_name: str, width: float = 0, height: float = 0,
        size_rule: str = "", fill_ratio: float = 1.0,
    ) -> str:
        """Set desired size or fill rules on a widget."""
        params = {"asset_path": asset_path, "widget_name": widget_name}
        if width > 0:
            params["width"] = width
        if height > 0:
            params["height"] = height
        if size_rule:
            params["size_rule"] = size_rule
        if fill_ratio != 1.0:
            params["fill_ratio"] = fill_ratio
        try:
            response = connection.send_command("umg.set_size", params)
            return format_response(response.get("data", {}), "set_size")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_visibility(asset_path: str, widget_name: str, visibility: str) -> str:
        """Set widget visibility state."""
        try:
            response = connection.send_command("umg.set_visibility", {
                "asset_path": asset_path,
                "widget_name": widget_name,
                "visibility": visibility,
            })
            return format_response(response.get("data", {}), "set_visibility")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_property(asset_path: str, widget_name: str, property_path: str, value: str) -> str:
        """Set any widget property via reflection path."""
        params = {
            "asset_path": asset_path,
            "widget_name": widget_name,
            "property_path": property_path,
        }
        try:
            params["value"] = json.loads(value)
        except (json.JSONDecodeError, TypeError):
            params["value"] = value
        try:
            response = connection.send_command("umg.set_property", params)
            return format_response(response.get("data", {}), "set_property")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_property(asset_path: str, widget_name: str, property_path: str) -> str:
        """Read any widget property value via reflection path."""
        try:
            response = connection.send_command("umg.get_property", {
                "asset_path": asset_path,
                "widget_name": widget_name,
                "property_path": property_path,
            })
            return format_response(response.get("data", {}), "get_property")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_schema(asset_path: str, widget_name: str, category: str = "") -> str:
        """Get editable property schema for a widget."""
        params = {"asset_path": asset_path, "widget_name": widget_name}
        if category:
            params["category"] = category
        try:
            response = connection.send_command_cached(
                "umg.get_schema", params, ttl=_TTL_SCHEMA
            )
            return format_response(response.get("data", {}), "get_schema")
        except ConnectionError as e:
            return f"Error: {e}"
