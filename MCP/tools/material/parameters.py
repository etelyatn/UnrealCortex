"""MCP tools for material parameter operations."""

import json
import logging
from typing import Any
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_material_parameter_tools(mcp, connection: UEConnection):
    """Register all material parameter-related MCP tools."""

    @mcp.tool()
    def list_parameters(asset_path: str) -> str:
        """List all parameters on a material or material instance.

        Lists all parameters (scalar, vector, texture) available on the material
        or material instance. For instances, this includes inherited parameters
        from the parent material.

        Args:
            asset_path: Full asset path to the material or instance

        Returns:
            JSON with parameter arrays:
            - scalar: Array of scalar parameters (each with name and type)
            - vector: Array of vector parameters
            - texture: Array of texture parameters
        """
        try:
            params = {"asset_path": asset_path}
            result = connection.send_command("material.list_parameters", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"list_parameters failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def get_parameter(
        asset_path: str,
        parameter_name: str,
    ) -> str:
        """Get the value of a specific parameter.

        Retrieves the current value of a parameter on a material or instance.
        Automatically detects parameter type (scalar/vector/texture).

        Args:
            asset_path: Full asset path to the material or instance
            parameter_name: Name of the parameter to retrieve

        Returns:
            JSON with parameter details:
            - name: Parameter name
            - type: Parameter type (scalar, vector, or texture)
            - value: Current value (float for scalar, array [R,G,B,A] for vector, path string for texture)
        """
        try:
            params = {
                "asset_path": asset_path,
                "parameter_name": parameter_name,
            }
            result = connection.send_command("material.get_parameter", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"get_parameter failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def set_parameter(
        asset_path: str,
        parameter_name: str,
        parameter_type: str,
        value: Any,
    ) -> str:
        """Set a parameter value on a material instance.

        Sets or overrides a parameter value on a material instance.
        Only works on instances, not base materials.

        Args:
            asset_path: Full asset path to the material instance
            parameter_name: Name of the parameter to set
            parameter_type: Type of parameter ("scalar", "vector", or "texture")
            value: New value:
                   - For scalar: float number
                   - For vector: array of 4 floats [R, G, B, A]
                   - For texture: full asset path to texture

        Returns:
            JSON with:
            - parameter_name: Name of set parameter
            - parameter_type: Type of parameter
            - success: True if successful
        """
        try:
            params = {
                "asset_path": asset_path,
                "parameter_name": parameter_name,
                "parameter_type": parameter_type,
                "value": value,
            }
            result = connection.send_command("material.set_parameter", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"set_parameter failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def set_parameters(
        asset_path: str,
        parameters: list[dict[str, Any]],
    ) -> str:
        """Set multiple parameter values on a material instance in batch.

        Efficiently sets multiple parameters in a single operation.
        Only works on instances, not base materials.

        Args:
            asset_path: Full asset path to the material instance
            parameters: Array of parameter objects, each containing:
                        - parameter_name: Name of the parameter
                        - parameter_type: Type ("scalar", "vector", or "texture")
                        - value: New value (type depends on parameter_type)

        Returns:
            JSON with:
            - success_count: Number of successfully set parameters
            - total_count: Total number of parameters attempted
            - errors: Array of error messages (if any failures occurred)
        """
        try:
            params = {
                "asset_path": asset_path,
                "parameters": parameters,
            }
            result = connection.send_command("material.set_parameters", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"set_parameters failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def reset_parameter(
        asset_path: str,
        parameter_name: str,
    ) -> str:
        """Reset a parameter override on a material instance to parent value.

        Removes the instance's override for a parameter, causing it to inherit
        the value from its parent material. Only works on instances.

        Args:
            asset_path: Full asset path to the material instance
            parameter_name: Name of the parameter to reset

        Returns:
            JSON with:
            - parameter_name: Name of reset parameter
            - reset: True if successful
        """
        try:
            params = {
                "asset_path": asset_path,
                "parameter_name": parameter_name,
            }
            result = connection.send_command("material.reset_parameter", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"reset_parameter failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})
