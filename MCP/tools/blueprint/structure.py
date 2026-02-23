"""MCP tools for Blueprint structure operations (variables, functions)."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_blueprint_structure_tools(mcp, connection: UEConnection):
    """Register all Blueprint structure-related MCP tools."""

    @mcp.tool()
    def add_blueprint_variable(
        asset_path: str,
        name: str,
        type: str,
        default_value: str = "",
        is_exposed: bool = False,
        category: str = "",
    ) -> str:
        """Add a variable to a Blueprint.

        Args:
            asset_path: Full path to the Blueprint
            name: Variable name (e.g., 'Health')
            type: Variable type (bool, int, int32, float, double, FString, string, FName, name,
                  FText, text, FVector, vector, FRotator, rotator, FLinearColor, or class name)
            default_value: Optional default value as string (e.g., '100.0')
            is_exposed: If true, make the variable editable in editor (default: false)
            category: Optional category for organization in editor

        Returns:
            JSON with:
            - added: true if successful
            - name: Variable name
            - type: Variable type
            - default_value: Default value if specified
        """
        try:
            params = {
                "asset_path": asset_path,
                "name": name,
                "type": type,
            }
            if default_value:
                params["default_value"] = default_value
            if is_exposed:
                params["is_exposed"] = True
            if category:
                params["category"] = category
            response = connection.send_command("bp.add_variable", params)
            return format_response(response.get("data", {}), "add_blueprint_variable")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def remove_blueprint_variable(asset_path: str, name: str) -> str:
        """Remove a variable from a Blueprint.

        Args:
            asset_path: Full path to the Blueprint
            name: Variable name to remove

        Returns:
            JSON with:
            - removed: true if successful
            - name: Variable name that was removed
        """
        try:
            params = {
                "asset_path": asset_path,
                "name": name,
            }
            response = connection.send_command("bp.remove_variable", params)
            return format_response(response.get("data", {}), "remove_blueprint_variable")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def add_blueprint_function(
        asset_path: str,
        name: str,
        is_pure: bool = False,
        access: str = "Public",
        inputs: list = None,
        outputs: list = None,
    ) -> str:
        """Add a function to a Blueprint.

        Args:
            asset_path: Full path to the Blueprint
            name: Function name (e.g., 'CalculateDamage')
            is_pure: If true, make the function pure (no side effects) (default: false)
            access: Function access level: Public or Private (default: Public)
            inputs: Optional list of input parameters. Each item should be a dict with:
                   - name: Parameter name
                   - type: Parameter type
            outputs: Optional list of output parameters. Each item should be a dict with:
                    - name: Return value name
                    - type: Return value type

        Returns:
            JSON with:
            - added: true if successful
            - name: Function name
            - graph_name: Internal graph name
            - inputs: Input parameters if specified
            - outputs: Output parameters if specified
        """
        try:
            params = {
                "asset_path": asset_path,
                "name": name,
            }
            if is_pure:
                params["is_pure"] = True
            if access and access != "Public":
                params["access"] = access
            if inputs:
                params["inputs"] = inputs
            if outputs:
                params["outputs"] = outputs
            response = connection.send_command("bp.add_function", params)
            return format_response(response.get("data", {}), "add_blueprint_function")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def configure_timeline(
        asset_path: str,
        timeline_name: str,
        length: float = 1.0,
        loop: bool = False,
        tracks: list[dict] | None = None,
    ) -> str:
        """Configure a Blueprint timeline's tracks and keyframes.

        Args:
            asset_path: Full path to the Blueprint
            timeline_name: Timeline variable name
            length: Timeline length in seconds
            loop: Whether timeline loops
            tracks: Optional list of tracks. Supported types: float, vector

        Returns:
            JSON with:
            - timeline_name: Name of the configured timeline
            - track_count: Total configured track count
            - length: Timeline length
            - loop: Looping state
        """
        try:
            params = {
                "asset_path": asset_path,
                "timeline_name": timeline_name,
                "length": length,
                "loop": loop,
            }
            if tracks:
                params["tracks"] = tracks
            response = connection.send_command("bp.configure_timeline", params)
            return format_response(response.get("data", {}), "configure_timeline")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_component_defaults(
        asset_path: str,
        component_name: str,
        properties: dict[str, str],
    ) -> str:
        """Set object-reference defaults on a Blueprint component template.

        Args:
            asset_path: Full path to the Blueprint
            component_name: Component name in the Blueprint Components panel
            properties: Map of property name to asset object path

        Returns:
            JSON with:
            - component_name: Target component
            - properties_set: Number of properties successfully applied
            - errors: Optional list of per-property failures
        """
        try:
            params = {
                "asset_path": asset_path,
                "component_name": component_name,
                "properties": properties,
            }
            response = connection.send_command("bp.set_component_defaults", params)
            return format_response(response.get("data", {}), "set_component_defaults")
        except ConnectionError as e:
            return f"Error: {e}"
