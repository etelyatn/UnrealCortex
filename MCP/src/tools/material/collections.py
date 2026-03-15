"""MCP tools for material parameter collection operations."""

import json
import logging
from typing import Any
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_material_collection_tools(mcp, connection: UEConnection):
    """Register all material parameter collection MCP tools."""

    @mcp.tool()
    def list_material_collections(
        path: str = "/Game/",
        recursive: bool = True,
    ) -> str:
        """List all material parameter collections in the project.

        Material parameter collections allow you to define global parameters
        that can be referenced by many materials simultaneously.

        Args:
            path: Content directory path to search (default: "/Game/")
            recursive: Whether to search subdirectories recursively (default: True)

        Returns:
            JSON with:
            - collections: Array of collection assets with name and asset_path
            - count: Total number of collections found
        """
        try:
            params = {"path": path, "recursive": recursive}
            result = connection.send_command("material.list_collections", params)
            return format_response(result.get("data", {}), "list_material_collections")
        except Exception as e:
            logger.error(f"list_material_collections failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def get_material_collection(asset_path: str) -> str:
        """Get detailed information about a material parameter collection.

        Args:
            asset_path: Full asset path to the collection

        Returns:
            JSON with collection details:
            - name: Collection name
            - asset_path: Full asset path
            - scalar_parameters: Array of scalar parameters with names and values
            - vector_parameters: Array of vector parameters with names and values
            - parameter_count: Total number of parameters
        """
        try:
            params = {"asset_path": asset_path}
            result = connection.send_command("material.get_collection", params)
            return format_response(result.get("data", {}), "get_material_collection")
        except Exception as e:
            logger.error(f"get_material_collection failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def create_material_collection(
        asset_path: str,
        name: str,
    ) -> str:
        """Create a new material parameter collection.

        Args:
            asset_path: Directory path where the collection will be created
            name: Name for the new collection (should start with MPC_)

        Returns:
            JSON with:
            - asset_path: Full path to created collection
            - name: Collection name
            - created: True if successful
        """
        try:
            params = {"asset_path": asset_path, "name": name}
            result = connection.send_command("material.create_collection", params)
            return format_response(result.get("data", {}), "create_material_collection")
        except Exception as e:
            logger.error(f"create_material_collection failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def delete_material_collection(asset_path: str) -> str:
        """Delete a material parameter collection.

        Args:
            asset_path: Full asset path to the collection to delete

        Returns:
            JSON with:
            - asset_path: Path of deleted collection
            - deleted: True if successful
        """
        try:
            params = {"asset_path": asset_path}
            result = connection.send_command("material.delete_collection", params)
            return format_response(result.get("data", {}), "delete_material_collection")
        except Exception as e:
            logger.error(f"delete_material_collection failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def add_collection_parameter(
        asset_path: str,
        parameter_name: str,
        parameter_type: str,
        default_value: Any,
    ) -> str:
        """Add a parameter to a material parameter collection.

        Adds a new scalar or vector parameter to the collection.
        Collections have a limit of 1024 total parameters.

        Args:
            asset_path: Full asset path to the collection
            parameter_name: Name for the new parameter
            parameter_type: Type of parameter ("scalar" or "vector")
            default_value: Default value - float for scalar, array [R,G,B,A] for vector

        Returns:
            JSON with:
            - parameter_name: Name of added parameter
            - parameter_type: Type of parameter
            - added: True if successful
        """
        try:
            params = {
                "asset_path": asset_path,
                "parameter_name": parameter_name,
                "parameter_type": parameter_type,
                "default_value": default_value,
            }
            result = connection.send_command("material.add_collection_parameter", params)
            return format_response(result.get("data", {}), "add_collection_parameter")
        except Exception as e:
            logger.error(f"add_collection_parameter failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def remove_collection_parameter(
        asset_path: str,
        parameter_name: str,
    ) -> str:
        """Remove a parameter from a material parameter collection.

        Args:
            asset_path: Full asset path to the collection
            parameter_name: Name of the parameter to remove

        Returns:
            JSON with:
            - parameter_name: Name of removed parameter
            - removed: True if successful
        """
        try:
            params = {
                "asset_path": asset_path,
                "parameter_name": parameter_name,
            }
            result = connection.send_command("material.remove_collection_parameter", params)
            return format_response(result.get("data", {}), "remove_collection_parameter")
        except Exception as e:
            logger.error(f"remove_collection_parameter failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def set_collection_parameter(
        asset_path: str,
        parameter_name: str,
        value: Any,
    ) -> str:
        """Set the value of a parameter in a material parameter collection.

        Updates the default value of an existing parameter in the collection.
        This affects all materials that reference this collection parameter.

        Args:
            asset_path: Full asset path to the collection
            parameter_name: Name of the parameter to update
            value: New value - float for scalar, array [R,G,B,A] for vector

        Returns:
            JSON with:
            - parameter_name: Name of updated parameter
            - value: New value
            - updated: True if successful
        """
        try:
            params = {
                "asset_path": asset_path,
                "parameter_name": parameter_name,
                "value": value,
            }
            result = connection.send_command("material.set_collection_parameter", params)
            return format_response(result.get("data", {}), "set_collection_parameter")
        except Exception as e:
            logger.error(f"set_collection_parameter failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})
