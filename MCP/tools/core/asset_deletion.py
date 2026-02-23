"""MCP tools for asset and folder deletion."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_core_asset_deletion_tools(mcp, connection: UEConnection):
    """Register core asset deletion MCP tools."""

    @mcp.tool()
    def delete_asset(asset_path: str) -> str:
        """Delete a single asset by path.

        Permanently deletes an asset from the project. Handles file cleanup
        and object destruction. Will fail if the asset has active references.

        Args:
            asset_path: Full asset path (e.g., '/Game/Materials/M_Test')

        Returns:
            JSON with deleted (bool) and asset_path.
        """
        try:
            response = connection.send_command("core.delete_asset", {
                "asset_path": asset_path,
            })
            return format_response(response.get("data", {}), "delete_asset")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def delete_folder(
        folder_path: str,
        recursive: bool = True,
    ) -> str:
        """Delete all assets in a folder.

        Enumerates all assets in the specified folder and deletes them.
        By default operates recursively on all subfolders.

        Args:
            folder_path: Content folder path (e.g., '/Game/Temp/MyFolder')
            recursive: Whether to include subfolders. Defaults to True.

        Returns:
            JSON with deleted_count, folder_path, and assets array.
        """
        try:
            response = connection.send_command("core.delete_folder", {
                "folder_path": folder_path,
                "recursive": recursive,
            })
            return format_response(response.get("data", {}), "delete_folder")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
