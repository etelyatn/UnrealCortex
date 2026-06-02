"""MCP tools for localization and StringTable operations."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_LIST = 300  # 5 min


def register_localization_tools(mcp, connection: UEConnection):
    """Register all localization-related MCP tools."""

    @mcp.tool()
    def list_string_tables(path_filter: str = "") -> str:
        """List StringTable assets loaded in the Unreal Editor.

        Returns each table's name, path, and entry count.
        Use this to discover available StringTables before reading translations.

        Args:
            path_filter: Optional prefix filter for asset paths (e.g., '/Game/Ripper/Localization/').
                         Only returns tables whose path starts with this prefix.

        Returns:
            JSON with 'string_tables' array, each containing:
            - name: Asset name (e.g., 'ST_UI_MainMenu')
            - path: Full asset path
            - namespace: The StringTable's namespace identifier
        """
        try:
            params = {}
            if path_filter:
                params["path_filter"] = path_filter
            response = connection.send_command_cached(
                "data.list_string_tables", params, ttl=_TTL_LIST
            )
            return format_response(response.get("data", {}), "list_string_tables")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_translations(string_table_path: str, key_pattern: str = "") -> str:
        """Get translation entries from a StringTable.

        Use list_string_tables to discover available tables and their paths.

        Args:
            string_table_path: Full asset path to the StringTable
                               (e.g., '/Game/Ripper/Localization/ST_UI_MainMenu.ST_UI_MainMenu').
            key_pattern: Optional wildcard pattern to filter keys (e.g., 'Menu_*', '*_Title').
                         Uses Unreal wildcard matching (* for any chars, ? for single char).
                         Leave empty to return all entries.

        Returns:
            JSON with:
            - string_table_path: The requested path
            - entries: Array of {key, source_string} objects
            - count: Total number of matching entries
        """
        try:
            params = {"string_table_path": string_table_path}
            if key_pattern:
                params["key_pattern"] = key_pattern
            response = connection.send_command("data.get_translations", params)
            return format_response(response.get("data", {}), "get_translations")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_translation(string_table_path: str, key: str, text: str) -> str:
        """Set a translation entry in a StringTable.

        Creates the key if it does not exist, or updates the text if it does.

        Note: This marks the StringTable as dirty (unsaved). Use Unreal's File > Save All to persist.

        Args:
            string_table_path: Full asset path to the StringTable.
            key: The translation key (e.g., 'Menu_Play_Button').
            text: The translated text value (e.g., 'Start Game').

        Returns:
            JSON with success status, key, and whether it was created or updated.
        """
        try:
            response = connection.send_command("data.set_translation", {
                "string_table_path": string_table_path,
                "key": key,
                "text": text,
            })
            return format_response(response.get("data", {}), "set_translation")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def update_string_table(
        string_table_path: str,
        operations_json: str,
        dry_run: bool,
        save: bool = False,
        verbose: bool = False,
    ) -> str:
        """Batch mutate StringTable entries with an ordered operation list.

        Supported operation objects:
        - {"type":"set","key":"fireball.title","source_string":"Fireball"}
        - {"type":"rename","old_key":"entry.fireball.title","new_key":"fireball.title"}
        - {"type":"copy","old_key":"entry.fireball.body","new_key":"fireball.body_copy"}
        - {"type":"delete","key":"entry.fireball.title"}
        - {"type":"replace_all","old_prefix":"entry.","new_prefix":""}

        Use dry_run=True first for migrations. Compact responses include renamed,
        collisions, missing_keys, invalid_operations, operation_results, and summary counts.
        Pass verbose=True to include copied/set/deleted/replaced arrays.
        For structured object params, prefer data_cmd with command="update_string_table"
        and params.operations as an array.

        Args:
            string_table_path: Full path to the StringTable asset.
            operations_json: JSON array string of ordered operation objects.
            dry_run: Preview without writing. Required; pass false to apply.
            save: Save the package after mutation. Ignored for dry runs.
            verbose: Include full per-key mutation arrays.

        Returns:
            JSON mutation report with before/after key counts and audit arrays.
        """
        try:
            operation_data = json.loads(operations_json)
            params = {
                "string_table_path": string_table_path,
                "operations": operation_data,
                "dry_run": dry_run,
                "save": save,
                "verbose": verbose,
            }
            response = connection.send_command("data.update_string_table", params)
            if not dry_run:
                connection.invalidate_cache("data.list_string_tables:")
                connection.invalidate_cache("data.get_data_catalog:")
            return format_response(response.get("data", {}), "update_string_table")
        except json.JSONDecodeError as e:
            return json.dumps({"_error": "INVALID_JSON", "_message": f"Invalid operations_json: {e}"})
        except ConnectionError as e:
            return f"Error: {e}"
