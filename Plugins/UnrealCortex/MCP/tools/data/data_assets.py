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
