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
        """Save asset(s) to disk."""
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
        """Open asset(s) in Unreal Editor."""
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
        """Close asset editor tab(s)."""
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
        """Discard changes and reload asset(s) from disk."""
        try:
            params = {"asset_path": asset_path, "dry_run": dry_run}
            response = connection.send_command("core.reload_asset", params)
            return format_response(response.get("data", {}), "reload_asset")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
