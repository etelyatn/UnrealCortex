"""MCP tools for Blueprint Class Default Object (CDO) property operations."""

import logging

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def register_blueprint_class_defaults_tools(mcp, connection: UEConnection):
    """Register Blueprint class defaults MCP tools."""

    @mcp.tool()
    def get_class_defaults(
        blueprint_path: str,
        properties: list[str] | None = None,
    ) -> str:
        """Read default property values from a Blueprint's Class Default Object (CDO).

        Returns type information, current values, and property metadata.
        Call with no property names to discover all settable properties on a Blueprint.
        For reading properties on placed actor instances in a level, use get_actor_property instead.

        Args:
            blueprint_path: Full asset path to the Blueprint (e.g., '/Game/Blueprints/BP_Character')
            properties: Optional list of property names to read. Omit or pass empty list
                to discover all settable properties with their types and categories.

        Returns:
            JSON with:
            - blueprint_path: Asset path
            - class: Generated class name
            - parent_class: Parent class name
            - properties: Object mapping property names to type, value, category, and defined_in
            - count: Number of properties returned
        """
        try:
            params = {"blueprint_path": blueprint_path}
            if properties:
                params["properties"] = properties
            response = connection.send_command("bp.get_class_defaults", params)
            return format_response(response.get("data", {}), "get_class_defaults")
        except ConnectionError as exc:
            return f"Error: {exc}"

    @mcp.tool()
    def set_class_defaults(
        blueprint_path: str,
        properties: dict,
        compile: bool = True,
        save: bool = True,
    ) -> str:
        """Set default property values on a Blueprint's Class Default Object (CDO).

        Configures inherited C++ UPROPERTY defaults and Blueprint variable defaults.
        Object reference properties accept asset path strings (e.g., '/Game/Sim/Input/IA_Move').
        Supports batch setting of multiple properties in one call.
        For setting properties on placed actor instances in a level, use set_actor_property instead.

        Args:
            blueprint_path: Full asset path to the Blueprint (e.g., '/Game/Blueprints/BP_Character')
            properties: Dictionary mapping property names to their new values.
            compile: Auto-compile the Blueprint after setting properties (default: true)
            save: Auto-save the Blueprint to disk after setting properties (default: true)

        Returns:
            JSON with:
            - blueprint_path: Asset path
            - results: Per-property results with type, previous_value, new_value, success
            - compiled: Whether compilation was performed and succeeded
            - saved: Whether save was performed and succeeded
            - compile_errors: Array of compilation errors (if any)
            - warnings: Array of warning messages
        """
        try:
            params = {
                "blueprint_path": blueprint_path,
                "properties": properties,
                "compile": compile,
                "save": save,
            }
            response = connection.send_command("bp.set_class_defaults", params)
            return format_response(response.get("data", {}), "set_class_defaults")
        except ConnectionError as exc:
            return f"Error: {exc}"
