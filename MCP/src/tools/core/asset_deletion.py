"""MCP tools for asset and folder deletion."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_core_asset_deletion_tools(mcp, connection: UEConnection):
    """Register core asset deletion MCP tools."""

    @mcp.tool()
    def delete_asset(asset_path: str, force: bool = False) -> str:
        """Delete a single asset by path.

        Deletes the asset in memory, then removes its package file. Refuses
        paths outside writable content mounts. Without force, refuses with
        HAS_REFERENCES (details.referencers) when other packages reference it;
        with force, loaded references are nulled.

        Args:
            asset_path: Full asset path (e.g., '/Game/Materials/M_Test')
            force: Delete even when other packages reference the asset.

        Returns:
            JSON with deleted (bool) and asset_path.
        """
        try:
            response = connection.send_command("core.delete_asset", {
                "asset_path": asset_path,
                "force": force,
            })
            return format_response(response.get("data", {}), "delete_asset")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def delete_folder(
        folder_path: str,
        recursive: bool = True,
        force: bool = False,
    ) -> str:
        """Delete all assets in a folder.

        Deletes every asset in the folder in one batch, then their package
        files. Refuses non-writable mounts and content roots such as /Game.
        Without force, refuses with HAS_REFERENCES when packages outside the
        folder reference its assets; nothing is deleted in that case.

        Args:
            folder_path: Content folder path (e.g., '/Game/Temp/MyFolder')
            recursive: Whether to include subfolders. Defaults to True.
            force: Delete even when packages outside the folder reference its assets.

        Returns:
            JSON with deleted_count, folder_path, assets array and, on partial
            failure, a failed array.
        """
        try:
            response = connection.send_command("core.delete_folder", {
                "folder_path": folder_path,
                "recursive": recursive,
                "force": force,
            })
            return format_response(response.get("data", {}), "delete_folder")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
