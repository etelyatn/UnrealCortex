"""MCP tools for generic asset operations (save, open, close, reload)."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_core_asset_tools(mcp, connection: UEConnection):
    """Register core asset editor MCP tools."""

    @mcp.tool()
    def save_asset(
        asset_path: str | list[str],
        force: bool = False,
        dry_run: bool = False,
    ) -> str:
        """Save asset(s) to disk.

        Persists in-memory changes to the .uasset file on disk. By default only
        saves dirty (modified) assets. Supports single paths, arrays, and glob
        patterns (e.g., '/Game/Data/*').

        Args:
            asset_path: Asset path or list of paths to save. Supports glob patterns
                        with '*' wildcard (e.g., '/Game/Data/DT_*').
            force: Save even if the asset is not dirty. Defaults to False.
            dry_run: Preview which assets would be saved without writing to disk.

        Returns:
            JSON with 'results' array, each containing:
            - asset_path: Full object path of the asset
            - asset_type: UClass name (e.g., 'DataTable', 'Blueprint')
            - was_dirty: Whether the asset had unsaved changes
            - saved: Whether the asset was written to disk
            - can_save: (dry_run only) Whether the asset would be saved
        """
        try:
            params = {"asset_path": asset_path, "force": force, "dry_run": dry_run}
            response = connection.send_command("core.save_asset", params)
            return format_response(response.get("data", {}), "save_asset")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def open_asset(
        asset_path: str | list[str],
        dry_run: bool = False,
    ) -> str:
        """Open asset(s) in Unreal Editor.

        Opens the asset editor tab for each specified asset. Works with any asset
        type (DataTable, Blueprint, Material, etc.).

        Args:
            asset_path: Asset path or list of paths to open. Supports glob patterns.
            dry_run: Preview which assets would be opened without actually opening them.

        Returns:
            JSON with 'results' array, each containing:
            - asset_path: Full object path of the asset
            - asset_type: UClass name
            - was_already_open: Whether the asset was already open in an editor tab
            - editor_opened: Whether the editor tab was successfully opened
            - would_open: (dry_run only) Whether the asset would be opened
        """
        try:
            params = {"asset_path": asset_path, "dry_run": dry_run}
            response = connection.send_command("core.open_asset", params)
            return format_response(response.get("data", {}), "open_asset")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def close_asset(
        asset_path: str | list[str],
        save: bool = False,
        dry_run: bool = False,
    ) -> str:
        """Close asset editor tab(s).

        Closes the editor tab for each specified asset. Optionally saves before
        closing. Assets that are not currently open will report closed=false.

        Args:
            asset_path: Asset path or list of paths to close. Supports glob patterns.
            save: Save the asset before closing if it has unsaved changes. Defaults to False.
            dry_run: Preview which assets would be closed without actually closing them.

        Returns:
            JSON with 'results' array, each containing:
            - asset_path: Full object path of the asset
            - asset_type: UClass name
            - was_dirty: Whether the asset had unsaved changes
            - was_open: Whether the asset was open in an editor tab
            - saved: Whether the asset was saved before closing
            - closed: Whether the editor tab was closed
            - would_close: (dry_run only) Whether the asset would be closed
            - would_save: (dry_run only) Whether the asset would be saved
        """
        try:
            params = {"asset_path": asset_path, "save": save, "dry_run": dry_run}
            response = connection.send_command("core.close_asset", params)
            return format_response(response.get("data", {}), "close_asset")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def reload_asset(
        asset_path: str | list[str],
        dry_run: bool = False,
    ) -> str:
        """Discard changes and reload asset(s) from disk.

        Reverts in-memory state to match the on-disk .uasset file. Closes any open
        editor tabs before reloading, then reopens them if they were open. Assets
        without a saved package on disk will report an error.

        Args:
            asset_path: Asset path or list of paths to reload. Supports glob patterns.
            dry_run: Preview which assets would be reloaded without actually reloading them.

        Returns:
            JSON with 'results' array, each containing:
            - asset_path: Full object path of the asset
            - asset_type: UClass name
            - was_dirty: Whether the asset had unsaved changes
            - reloaded: Whether the asset was successfully reloaded from disk
            - discarded_changes: Whether unsaved changes were discarded
            - has_disk_file: (dry_run only) Whether the asset has a saved package on disk
        """
        try:
            params = {"asset_path": asset_path, "dry_run": dry_run}
            response = connection.send_command("core.reload_asset", params)
            return format_response(response.get("data", {}), "reload_asset")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
