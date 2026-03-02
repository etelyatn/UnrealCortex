"""MCP tool for searching Blueprint content."""

import json

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection


def register_blueprint_search_tools(mcp, connection: UEConnection):
    """Register Blueprint search tools."""

    @mcp.tool()
    def blueprint_search(
        asset_path: str,
        query: str,
        search_in: list[str] | None = None,
        case_sensitive: bool = False,
        max_results: int = 100,
    ) -> str:
        """Search a Blueprint for values across all graphs, class defaults, and widget tree.

        Finds text, strings, names, and object references that match the query.
        Searches resolved text, StringTable IDs, and StringTable keys.

        Use this tool when you need to find where a specific value, string table key,
        or object reference is used within a Blueprint.

        Args:
            asset_path: Blueprint asset path (for example '/Game/UI/WBP_MainMenu').
            query: Text to search for (substring match).
            search_in: Optional list to limit scope: 'pins', 'cdo', 'widgets'.
            case_sensitive: Whether search is case-sensitive.
            max_results: Maximum matches to return.

        Returns:
            JSON with match_count, truncated flag, and matches array. Each match has:
            - location: Human-readable path
            - node_id: Machine-readable node ID for graph matches
            - graph_name: Graph name for graph matches
            - property: Property or pin name
            - type: 'pin', 'cdo', or 'widget'
            - value: The matched value
            - string_table: {table_id, key} when value is StringTable-backed
        """
        try:
            params = {
                "asset_path": asset_path,
                "query": query,
                "case_sensitive": case_sensitive,
                "max_results": max_results,
            }
            if search_in:
                params["search_in"] = search_in

            response = connection.send_command("bp.search", params)
            return format_response(response.get("data", {}), "blueprint_search")
        except ConnectionError as exc:
            return json.dumps({"error": f"Connection error: {exc}"})
        except (RuntimeError, TimeoutError, OSError) as exc:
            return json.dumps({"error": str(exc)})
