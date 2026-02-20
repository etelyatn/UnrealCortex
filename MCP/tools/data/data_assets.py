"""MCP tools for DataAsset read and write operations."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_LIST = 300  # 5 min


def register_data_asset_tools(mcp, connection: UEConnection):
    """Register all DataAsset-related MCP tools."""

    @mcp.tool()
    def list_data_assets(class_filter: str = "", path_filter: str = "") -> str:
        """List DataAssets currently loaded in the Unreal Editor.

        Returns each asset's name, path, and class type.
        Use this to discover available DataAssets before reading their properties.

        Args:
            class_filter: Optional class name to filter by (e.g., 'RipProductDataAsset').
                          Only returns assets of this class or its subclasses.
            path_filter: Optional prefix filter for asset paths (e.g., '/Game/Ripper/Products/').
                         Only returns assets whose path starts with this prefix.

        Returns:
            JSON with 'data_assets' array, each containing:
            - name: Asset name (e.g., 'DA_CyberArm_Mk1')
            - path: Full asset path
            - class_name: UClass name of the DataAsset
        """
        try:
            params = {}
            if class_filter:
                params["class_filter"] = class_filter
            if path_filter:
                params["path_filter"] = path_filter
            response = connection.send_command_cached(
                "data.list_data_assets", params, ttl=_TTL_LIST
            )
            return format_response(response.get("data", {}), "list_data_assets")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_data_asset(asset_path: str) -> str:
        """Read all properties of a DataAsset.

        Use list_data_assets to discover available assets and their paths.

        Args:
            asset_path: Full asset path to the DataAsset
                        (e.g., '/Game/Ripper/Products/DA_CyberArm_Mk1.DA_CyberArm_Mk1').

        Returns:
            JSON with:
            - asset_path: The requested asset path
            - asset_class: UClass name of the DataAsset
            - properties: Object with all property names and values
        """
        try:
            response = connection.send_command("data.get_data_asset", {
                "asset_path": asset_path,
            })
            return format_response(response.get("data", {}), "get_data_asset")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def update_data_asset(asset_path: str, properties: str, dry_run: bool = False) -> str:
        """Update properties on a DataAsset.

        Only properties present in the JSON are modified; other properties remain unchanged.
        Use get_data_asset first to see current values and property names.

        Note: This marks the asset as dirty (unsaved). Use Unreal's File > Save All to persist.

        Args:
            asset_path: Full asset path to the DataAsset.
            properties: JSON string with property names and values to update.
                        Example: '{"DisplayName": "Cyber Arm Mk2", "BaseCost": 5000}'
            dry_run: If true, preview changes without applying them. Returns 'changes' array with
                     {field, old_value, new_value} for each modified property. Default: false.

        Returns:
            When dry_run=false (normal mode):
            - JSON with success status, modified_fields list, and any warnings.

            When dry_run=true (preview mode):
            - JSON with dry_run=true, changes array with {field, old_value, new_value} for each change.
            - No modifications are applied to the actual DataAsset.
        """
        try:
            props = json.loads(properties)
            response = connection.send_command("data.update_data_asset", {
                "asset_path": asset_path,
                "properties": props,
                "dry_run": dry_run,
            })
            return format_response(response.get("data", {}), "update_data_asset")
        except json.JSONDecodeError as e:
            return f"Error: Invalid JSON in properties: {e}"
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def create_data_asset(class_name: str, asset_path: str, properties: str = "") -> str:
        """Create a new DataAsset of a specified class.

        Creates a new DataAsset instance at the given path, optionally setting
        initial property values. The asset is saved to disk immediately.

        Use list_data_assets to discover available DataAsset class names
        (returned in the asset_class field of each entry).

        Args:
            class_name: UClass name of the DataAsset to create (e.g., 'SimMotorDataAsset').
                        Accepts short names or full class paths. Must be a concrete (non-abstract)
                        subclass of UDataAsset.
            asset_path: Target package path (e.g., '/Game/Data/Parts/DA_Motor_2306').
                        Both package path and full object path formats are accepted.
            properties: Optional JSON string with initial property values (same format as
                        update_data_asset). Example: '{"DisplayName": "New Motor", "KV": 2750}'

        Returns:
            JSON with:
            - asset_path: Full object path of the created asset (use this with get/update/delete)
            - asset_class: Resolved UClass name
            - created: true
        """
        try:
            params = {
                "class_name": class_name,
                "asset_path": asset_path,
            }
            if properties:
                props = json.loads(properties)
                params["properties"] = props
            response = connection.send_command("data.create_data_asset", params)
            connection.invalidate_cache("data.list_data_assets:")
            connection.invalidate_cache("data.get_data_catalog:")
            return format_response(response.get("data", {}), "create_data_asset")
        except json.JSONDecodeError as e:
            return f"Error: Invalid JSON in properties: {e}"
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def delete_data_asset(asset_path: str) -> str:
        """Delete a DataAsset by path.

        Permanently removes the DataAsset from both memory and disk.
        Use list_data_assets to find the asset path before deletion.

        Args:
            asset_path: Asset path of the DataAsset to delete.
                        Both package path (e.g., '/Game/Data/Parts/DA_Motor_2306')
                        and full object path (e.g., '/Game/Data/Parts/DA_Motor_2306.DA_Motor_2306')
                        formats are accepted.

        Returns:
            JSON with:
            - asset_path: Path of the deleted asset
            - deleted: true
        """
        try:
            response = connection.send_command("data.delete_data_asset", {
                "asset_path": asset_path,
            })
            connection.invalidate_cache("data.list_data_assets:")
            connection.invalidate_cache("data.get_data_catalog:")
            return format_response(response.get("data", {}), "delete_data_asset")
        except ConnectionError as e:
            return f"Error: {e}"
