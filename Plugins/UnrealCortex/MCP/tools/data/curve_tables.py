"""MCP tools for CurveTable operations."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_LIST = 300  # 5 min


def register_curve_table_tools(mcp, connection: UEConnection):
    """Register all CurveTable-related MCP tools."""

    @mcp.tool()
    def list_curve_tables(path_filter: str = "") -> str:
        """List all CurveTables currently loaded in the Unreal Editor.

        CurveTables store float curves (e.g., XP scaling, damage falloff, animation blending).
        Each row is a named curve with time/value key pairs.

        Args:
            path_filter: Optional prefix filter for asset paths (e.g., '/Game/Data/Curves/').

        Returns:
            JSON with:
            - curve_tables: Array of {name, path, row_count, curve_type}
            - count: Total number of matching CurveTables
        """
        try:
            params = {}
            if path_filter:
                params["path_filter"] = path_filter
            response = connection.send_command_cached(
                "data.list_curve_tables", params, ttl=_TTL_LIST
            )
            return format_response(response.get("data", {}), "list_curve_tables")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_curve_table(table_path: str, row_name: str = "") -> str:
        """Get curves from a CurveTable, optionally filtered to a single row.

        Each curve row contains an array of time/value keys defining the curve shape.
        RichCurve keys also include interpolation mode (Linear, Cubic, Constant, etc.).

        Args:
            table_path: Full asset path to the CurveTable.
            row_name: Optional row name to get a single curve. Leave empty for all curves.

        Returns:
            JSON with:
            - table_path: The queried CurveTable
            - curves: Array of {row_name, curve_type, keys[], key_count}
            - count: Number of curves returned
        """
        try:
            params = {"table_path": table_path}
            if row_name:
                params["row_name"] = row_name
            response = connection.send_command("data.get_curve_table", params)
            return format_response(response.get("data", {}), "get_curve_table")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def update_curve_table_row(table_path: str, row_name: str, keys: str) -> str:
        """Replace all keys in a CurveTable row with new time/value pairs.

        This replaces the entire curve. Use get_curve_table first to read current keys,
        then modify and pass the full set back.

        Note: This marks the CurveTable as dirty (unsaved). Use Unreal's File > Save All to persist.

        Args:
            table_path: Full asset path to the CurveTable.
            row_name: Name of the curve row to update.
            keys: JSON string with array of {time, value} objects.
                  Example: '[{"time": 0.0, "value": 1.0}, {"time": 10.0, "value": 100.0}]'

        Returns:
            JSON with success status, row_name, and keys_updated count.
        """
        try:
            keys_data = json.loads(keys)
            response = connection.send_command("data.update_curve_table_row", {
                "table_path": table_path,
                "row_name": row_name,
                "keys": keys_data,
            })
            return format_response(response.get("data", {}), "update_curve_table_row")
        except json.JSONDecodeError as e:
            return f"Error: Invalid JSON in keys: {e}"
        except ConnectionError as e:
            return f"Error: {e}"
