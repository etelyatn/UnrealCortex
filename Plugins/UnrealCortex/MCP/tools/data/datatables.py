"""MCP tools for DataTable read and write operations."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

# Cache TTLs (seconds)
_TTL_SCHEMA = 1800  # 30 min - schemas require recompile to change
_TTL_LIST = 300  # 5 min - table list rarely changes during session


def register_datatable_tools(mcp, connection: UEConnection):
    """Register all DataTable-related MCP tools."""

    @mcp.tool()
    def list_datatables(path_filter: str = "") -> str:
        """List all DataTables currently loaded in the Unreal Editor.

        Returns each table's name, asset path, row struct type, and row count.
        Use this to discover available DataTables before querying their contents.

        CompositeDataTables (tables that aggregate rows from multiple source tables)
        are flagged with is_composite=true and include a parent_tables array listing
        their source tables. Prefer reading from composites (they show all aggregated data),
        but write to the actual source tables.

        Args:
            path_filter: Optional prefix filter for asset paths (e.g., '/Game/Data/Quests/').
                         Only returns tables whose path starts with this prefix.

        Returns:
            JSON with 'datatables' array, each containing:
            - name: Asset name (e.g., 'DT_QuestDefinitions')
            - path: Full asset path (e.g., '/Game/Data/Quests/DT_QuestDefinitions.DT_QuestDefinitions')
            - row_struct: Name of the row struct type (e.g., 'FRipQuestDefinition')
            - row_count: Number of rows in the table
            - is_composite: Whether this is a CompositeDataTable
            - parent_tables: (composites only) Array of {name, path} for each source table
        """
        try:
            response = connection.send_command_cached(
                "data.list_datatables", {"path_filter": path_filter}, ttl=_TTL_LIST
            )
            return format_response(response.get("data", {}), "list_datatables")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_datatable_schema(table_path: str, include_inherited: bool = True) -> str:
        """Get the row struct schema for a DataTable, showing all fields, types, and constraints.

        Call this before reading or writing DataTable rows to understand the row structure.
        For TInstancedStruct fields, this shows the base struct and known subtypes.

        Args:
            table_path: Full asset path to the DataTable (e.g., '/Game/Data/Quests/DT_QuestDefinitions.DT_QuestDefinitions').
                        Use list_datatables to discover available paths.
            include_inherited: Whether to include fields inherited from parent structs (default: True).

        Returns:
            JSON with 'schema' containing field definitions, each with:
            - name: Field name
            - type: Human-readable type (e.g., 'int32', 'FString', 'TArray<FGameplayTag>')
            - cpp_type: C++ type name
            - enum_values: For enum fields, array of valid string values
            - element_type: For array fields, the element type schema
            - fields: For struct fields, nested field schemas
        """
        try:
            response = connection.send_command_cached(
                "data.get_datatable_schema",
                {"table_path": table_path, "include_inherited": include_inherited},
                ttl=_TTL_SCHEMA,
            )
            return format_response(response.get("data", {}), "get_datatable_schema")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def query_datatable(
        table_path: str,
        row_name_pattern: str = "",
        row_names: str = "",
        fields: str = "",
        limit: int = 25,
        offset: int = 0,
    ) -> str:
        """Query rows from a DataTable with optional filtering, field selection, and pagination.

        Use get_datatable_schema first to understand the row structure.

        For tables with complex nested structs, use 'fields' to select only needed
        fields. This skips serialization of heavy nested data (like InstancedStruct
        arrays) and dramatically reduces response size.

        Args:
            table_path: Full asset path to the DataTable.
            row_name_pattern: Optional wildcard pattern to filter row names (e.g., 'Quest_*', '*_Boss').
                              Uses Unreal wildcard matching (* for any chars, ? for single char).
            row_names: Optional comma-separated list of exact row names to fetch
                       (e.g., 'Quest_Tutorial_01,Quest_Build_01'). When provided,
                       row_name_pattern is ignored. Returns rows in requested order.
                       Missing names are reported in 'missing_rows'.
            fields: Optional comma-separated list of field names to include in results.
                    Leave empty to include all fields. Example: 'Title,QuestType,Priority'.
            limit: Maximum number of rows to return (default: 25). Ignored when row_names is set.
            offset: Number of rows to skip for pagination (default: 0). Ignored when row_names is set.

        Returns:
            JSON with:
            - rows: Array of {row_name, row_data} objects
            - total_count: Total matching rows (before pagination)
            - offset: Applied offset
            - limit: Applied limit
            - missing_rows: (when row_names used) Array of names not found in table
        """
        try:
            params = {
                "table_path": table_path,
                "limit": limit,
                "offset": offset,
            }
            if row_names:
                params["row_names"] = [n.strip() for n in row_names.split(",")]
            elif row_name_pattern:
                params["row_name_pattern"] = row_name_pattern
            if fields:
                params["fields"] = [f.strip() for f in fields.split(",")]
            response = connection.send_command("data.query_datatable", params)
            return format_response(response.get("data", {}), "query_datatable")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_datatable_row(table_path: str, row_name: str) -> str:
        """Get a specific row from a DataTable by its row name.

        Use list_datatables to find available tables, and query_datatable to discover row names.

        Args:
            table_path: Full asset path to the DataTable.
            row_name: The row name/key to look up (e.g., 'Quest_Tutorial_01').

        Returns:
            JSON with:
            - row_name: The requested row name
            - row_struct: The struct type name
            - row_data: Object with all row field values
        """
        try:
            response = connection.send_command("data.get_datatable_row", {
                "table_path": table_path,
                "row_name": row_name,
            })
            return format_response(response.get("data", {}), "get_datatable_row")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def get_struct_schema(struct_name: str, include_subtypes: bool = False) -> str:
        """Get the schema for any UStruct type by name, showing fields, types, and constraints.

        Use this to understand struct types used in DataTable rows, especially for
        TInstancedStruct fields that can hold different subtypes.

        Args:
            struct_name: Name of the struct (e.g., 'FRipQuestDefinition', 'FGameplayTag').
                         The 'F' prefix is optional — both 'RipQuestDefinition' and
                         'FRipQuestDefinition' will work.
            include_subtypes: For TInstancedStruct base types, include a list of known
                              subtypes and their schemas (default: False).

        Returns:
            JSON with 'schema' containing field definitions, and optionally 'subtypes' array.
        """
        try:
            response = connection.send_command_cached(
                "data.get_struct_schema",
                {"struct_name": struct_name, "include_subtypes": include_subtypes},
                ttl=_TTL_SCHEMA,
            )
            return format_response(response.get("data", {}), "get_struct_schema")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def add_datatable_row(table_path: str, row_name: str, row_data: str) -> str:
        """Add a new row to a DataTable.

        IMPORTANT: Cannot add rows to CompositeDataTables. If targeted at a composite,
        returns COMPOSITE_WRITE_BLOCKED error with the list of source tables to use instead.
        Use list_datatables to identify composites and their source tables.

        Use get_datatable_schema first to understand the expected row structure.
        For TInstancedStruct fields, include a '_struct_type' key specifying the concrete type name
        (e.g., '_struct_type': 'FRipRewardItem'). Use get_struct_schema with include_subtypes=True
        to discover valid subtypes.

        Note: This marks the DataTable as dirty (unsaved). Use Unreal's File > Save All to persist.

        Args:
            table_path: Full asset path to the DataTable. Must be a non-composite table.
            row_name: Unique name/key for the new row (e.g., 'Quest_NewQuest_01').
            row_data: JSON string with row field values. Example: '{"QuestName": "Test", "Difficulty": 3}'

        Returns:
            JSON with success status, row_name, and any warnings.
        """
        try:
            data = json.loads(row_data)
            response = connection.send_command("data.add_datatable_row", {
                "table_path": table_path,
                "row_name": row_name,
                "row_data": data,
            })
            # Invalidate list caches (row count changed)
            connection.invalidate_cache("data.list_datatables:")
            connection.invalidate_cache("data.get_data_catalog:")
            return format_response(response.get("data", {}), "add_datatable_row")
        except json.JSONDecodeError as e:
            return f"Error: Invalid JSON in row_data: {e}"
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def update_datatable_row(table_path: str, row_name: str, row_data: str, dry_run: bool = False) -> str:
        """Update an existing row in a DataTable with partial data.

        Composite-aware: If table_path points to a CompositeDataTable, the update
        automatically resolves to the actual source table that owns the row. The response
        includes source_table_path and composite_table_path when auto-resolution occurs.

        Only fields present in row_data are modified; other fields remain unchanged.
        Use get_datatable_row first to see current values. For TInstancedStruct fields,
        include a '_struct_type' key specifying the concrete type name
        (e.g., '_struct_type': 'FRipRewardItem').

        Note: This marks the DataTable as dirty (unsaved). Use Unreal's File > Save All to persist.

        Args:
            table_path: Full asset path to the DataTable (can be a composite — auto-resolves).
            row_name: Name of the existing row to update.
            row_data: JSON string with fields to update. Only specified fields are changed.
            dry_run: If true, preview changes without applying them. Returns 'changes' array with
                     {field, old_value, new_value} for each modified field. Default: false.

        Returns:
            When dry_run=false (normal mode):
            - JSON with success status, modified_fields list, and any warnings.
            - If auto-resolved from a composite, also includes source_table_path and composite_table_path.

            When dry_run=true (preview mode):
            - JSON with dry_run=true, changes array with {field, old_value, new_value} for each change.
            - No modifications are applied to the actual DataTable.
        """
        try:
            data = json.loads(row_data)
            response = connection.send_command("data.update_datatable_row", {
                "table_path": table_path,
                "row_name": row_name,
                "row_data": data,
                "dry_run": dry_run,
            })
            return format_response(response.get("data", {}), "update_datatable_row")
        except json.JSONDecodeError as e:
            return f"Error: Invalid JSON in row_data: {e}"
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def delete_datatable_row(table_path: str, row_name: str) -> str:
        """Delete a row from a DataTable.

        Composite-aware: If table_path points to a CompositeDataTable, the delete
        automatically resolves to the actual source table that owns the row. The response
        includes source_table_path and composite_table_path when auto-resolution occurs.

        Args:
            table_path: Full asset path to the DataTable (can be a composite — auto-resolves).
            row_name: Name of the row to delete.

        Returns:
            JSON with success status and deleted row_name.
            If auto-resolved from a composite, also includes source_table_path and composite_table_path.
        """
        try:
            response = connection.send_command("data.delete_datatable_row", {
                "table_path": table_path,
                "row_name": row_name,
            })
            # Invalidate list caches (row count changed)
            connection.invalidate_cache("data.list_datatables:")
            connection.invalidate_cache("data.get_data_catalog:")
            return format_response(response.get("data", {}), "delete_datatable_row")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def search_datatable_content(
        table_path: str,
        search_text: str,
        fields: str = "",
        preview_fields: str = "",
        limit: int = 20,
    ) -> str:
        """Search inside DataTable row field values for a case-insensitive substring match.

        Searches all string-like fields (FString, FName, FText) including one level of nested
        structs. For FText fields, searches the source/invariant string (English during dev).
        Works with both regular and CompositeDataTables (composites search all aggregated rows).

        Use this instead of query_datatable when row names are generic (e.g., 'GenericOrder_1')
        and you need to find rows by their content. Prefer composite tables (e.g., CQT_Quests)
        for broad searches across multiple source tables.

        Args:
            table_path: Full asset path to the DataTable or CompositeDataTable.
            search_text: Case-insensitive substring to search for in string field values.
            fields: Optional comma-separated field names to restrict the search to.
                    Supports dot-paths for nested structs (e.g., 'Message.BodyText').
                    Leave empty to search all string-like fields.
            preview_fields: Optional comma-separated field names to include in each result
                            for context (e.g., 'QuestTag,QuestType'). Top-level fields only.
            limit: Maximum number of matching rows to return (default: 20).

        Returns:
            JSON with:
            - table_path: The searched table
            - search_text: The search term used
            - total_matches: Number of matching rows found (capped at limit)
            - limit: Applied limit
            - results: Array of matching rows, each with:
              - row_name: The row key
              - matches: Array of {field, value} for each matching field
              - preview: (if preview_fields specified) Object with requested field values
        """
        try:
            params = {
                "table_path": table_path,
                "search_text": search_text,
                "limit": limit,
            }
            if fields:
                params["fields"] = [f.strip() for f in fields.split(",")]
            if preview_fields:
                params["preview_fields"] = [f.strip() for f in preview_fields.split(",")]
            response = connection.send_command("data.search_datatable_content", params)
            return format_response(response.get("data", {}), "search_datatable_content")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def import_datatable_json(table_path: str, rows: str, mode: str = "create", dry_run: bool = False) -> str:
        """Bulk import rows into a DataTable.

        IMPORTANT: Cannot import into CompositeDataTables. If targeted at a composite,
        returns COMPOSITE_WRITE_BLOCKED error with the list of source tables to use instead.
        Use list_datatables to identify composites and their source tables.

        Args:
            table_path: Full asset path to the DataTable. Must be a non-composite table.
            rows: JSON string with array of objects, each containing 'row_name' and 'row_data'.
                  Example: '[{"row_name": "Row1", "row_data": {"Field1": "value"}}]'
            mode: Import mode - 'create' (skip existing), 'upsert' (create or update),
                  or 'replace' (clear table first). Default: 'create'.
            dry_run: If true, validate without writing. Default: false.

        Returns:
            JSON with counts of created, updated, skipped rows, plus any errors/warnings.
        """
        try:
            rows_data = json.loads(rows)
            response = connection.send_command("data.import_datatable_json", {
                "table_path": table_path,
                "rows": rows_data,
                "mode": mode,
                "dry_run": dry_run,
            })
            if not dry_run:
                # Invalidate list caches (row counts changed)
                connection.invalidate_cache("data.list_datatables:")
                connection.invalidate_cache("data.get_data_catalog:")
            return format_response(response.get("data", {}), "import_datatable_json")
        except json.JSONDecodeError as e:
            return f"Error: Invalid JSON in rows: {e}"
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def batch_query(commands: str) -> str:
        """Execute multiple data queries in a single round-trip.

        Primary tool for "join" workflows: fetch a quest, then resolve all its
        referenced patients/jobs/products in one call. Max 20 commands per batch.

        Args:
            commands: JSON array of command objects, each with 'command' and optional 'params'.
                      Example: '[{"command": "ping"}, {"command": "data.get_datatable_row", "params": {"table_path": "/Game/...", "row_name": "Row1"}}]'

        Returns:
            JSON with:
            - results: Array of per-command results, each with index, command, success, data/error, timing_ms
            - count: Number of commands executed
            - total_timing_ms: Total batch execution time
        """
        try:
            cmds = json.loads(commands)
            response = connection.send_command("batch", {"commands": cmds})
            return format_response(response.get("data", {}), "batch_query")
        except json.JSONDecodeError as e:
            return f"Error: Invalid JSON in commands: {e}"
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def resolve_tags(table_path: str, tag_field: str, tags: str, fields: str = "") -> str:
        """Resolve GameplayTags to DataTable rows containing those tags.

        Use to follow tag references between tables. Example: quest has
        PatientTags ['Patient.NPC.Maria'] -> resolve against Patient table's
        PatientTag field to get actual patient data.

        Args:
            table_path: Full asset path to the target DataTable to search.
            tag_field: Name of the FGameplayTag or FGameplayTagContainer field to match against.
            tags: Comma-separated list of GameplayTag strings to resolve
                  (e.g., 'Patient.NPC.Maria,Patient.NPC.Viktor').
            fields: Optional comma-separated list of field names to include in results.
                    Leave empty for all fields. Example: 'PatientName,PatientTag'.

        Returns:
            JSON with:
            - resolved: Array of {row_name, row_data, matched_tags} for matching rows
            - resolved_count: Number of rows that matched
            - unresolved_tags: Tags that didn't match any row
        """
        try:
            params = {
                "table_path": table_path,
                "tag_field": tag_field,
                "tags": [t.strip() for t in tags.split(",")],
            }
            if fields:
                params["fields"] = [f.strip() for f in fields.split(",")]
            response = connection.send_command("data.resolve_tags", params)
            return format_response(response.get("data", {}), "resolve_tags")
        except ConnectionError as e:
            return f"Error: {e}"
