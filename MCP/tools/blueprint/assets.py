"""MCP tools for Blueprint asset operations."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_blueprint_asset_tools(mcp, connection: UEConnection):
    """Register all Blueprint asset-related MCP tools."""

    @mcp.tool()
    def create_blueprint(
        name: str,
        path: str,
        type: str = "Actor",
        parent_class: str = "",
    ) -> str:
        """Create a new Blueprint asset.

        Args:
            name: Blueprint name (e.g., 'BP_Character')
            path: Asset path directory (e.g., '/Game/Blueprints')
            type: Blueprint base type: Actor, Component, Widget, Interface, or FunctionLibrary
                 (default: Actor). Ignored when parent_class is provided.
            parent_class: Optional C++ class to use as Blueprint parent. Accepts short name
                 (e.g., 'CortexBenchmarkActor') or full path
                 (e.g., '/Script/CortexSandbox.CortexBenchmarkActor').
                 When provided, overrides the type parameter.

        Returns:
            JSON with:
            - asset_path: Full path to the created Blueprint
            - type: Confirmed type
            - parent_class: Parent class name
            - created: true if successful
        """
        try:
            params = {
                "name": name,
                "path": path,
                "type": type,
            }
            if parent_class:
                params["parent_class"] = parent_class
            response = connection.send_command("bp.create", params)
            return format_response(response.get("data", {}), "create_blueprint")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def list_blueprints(
        path: str = "",
        type: str = "",
    ) -> str:
        """List Blueprint assets in the project.

        Args:
            path: Optional path filter (e.g., '/Game/Blueprints')
            type: Optional type filter (Actor, Component, Widget, Interface, FunctionLibrary)

        Returns:
            JSON with:
            - blueprints: Array of Blueprint objects, each containing:
              - asset_path: Full path
              - name: Asset name
              - type: Blueprint type
        """
        try:
            params = {}
            if path:
                params["path"] = path
            if type:
                params["type"] = type
            response = connection.send_command("bp.list", params)
            return format_response(response.get("data", {}), "list_blueprints")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_blueprint_info(asset_path: str) -> str:
        """Get detailed information about a Blueprint.

        Args:
            asset_path: Full path to the Blueprint (e.g., '/Game/Blueprints/BP_Character')

        Returns:
            JSON with:
            - name: Blueprint name
            - asset_path: Full path
            - type: Blueprint type
            - parent_class: Parent class name
            - variables: Array of variables, each with name and type
            - functions: Array of functions, each with name
            - graphs: Array of all graphs
        """
        try:
            params = {"asset_path": asset_path}
            response = connection.send_command("bp.get_info", params)
            return format_response(response.get("data", {}), "get_blueprint_info")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def delete_blueprint(asset_path: str, force: bool = False) -> str:
        """Delete a Blueprint asset.

        Args:
            asset_path: Full path to the Blueprint
            force: If true, delete even if other assets reference it (default: false)

        Returns:
            JSON with:
            - deleted: true if successful
            - asset_path: Path of deleted asset
        """
        try:
            params = {"asset_path": asset_path}
            if force:
                params["force"] = True
            response = connection.send_command("bp.delete", params)
            return format_response(response.get("data", {}), "delete_blueprint")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def duplicate_blueprint(asset_path: str, new_name: str, new_path: str = "") -> str:
        """Duplicate a Blueprint asset.

        Args:
            asset_path: Full path to source Blueprint
            new_name: Name for the duplicate (e.g., 'BP_Character_2')
            new_path: Optional destination path (defaults to source directory)

        Returns:
            JSON with:
            - source: Original asset path
            - new_asset_path: Path to the new Blueprint
            - duplicated: true if successful
        """
        try:
            params = {
                "asset_path": asset_path,
                "new_name": new_name,
            }
            if new_path:
                params["new_path"] = new_path
            response = connection.send_command("bp.duplicate", params)
            return format_response(response.get("data", {}), "duplicate_blueprint")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def compile_blueprint(asset_path: str) -> str:
        """Compile a Blueprint.

        Args:
            asset_path: Full path to the Blueprint

        Returns:
            JSON with:
            - compiled: true if compilation succeeded
            - asset_path: Path of compiled Blueprint
            - warnings: Array of warning messages (if any)
        """
        try:
            params = {"asset_path": asset_path}
            response = connection.send_command("bp.compile", params)
            return format_response(response.get("data", {}), "compile_blueprint")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def save_blueprint(asset_path: str) -> str:
        """Save a Blueprint asset to disk.

        Args:
            asset_path: Full path to the Blueprint

        Returns:
            JSON with:
            - saved: true if successful
            - asset_path: Path of saved Blueprint
        """
        try:
            params = {"asset_path": asset_path}
            response = connection.send_command("bp.save", params)
            return format_response(response.get("data", {}), "save_blueprint")
        except ConnectionError as e:
            return f"Error: {e}"
