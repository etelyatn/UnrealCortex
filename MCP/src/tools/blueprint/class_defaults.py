"""MCP tools for Blueprint Class Default Object (CDO) property operations."""

import logging

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def register_blueprint_class_defaults_tools(mcp, connection: UEConnection):
    """Register Blueprint class defaults MCP tools."""

    @mcp.tool()
    def get_class_defaults(
        asset_path: str = "",
        properties: list[str] | None = None,
        blueprint_path: str = "",
        include_values: bool | None = None,
        include_members: bool | None = None,
        discovery_offset: int | None = None,
        discovery_limit: int | None = None,
    ) -> str:
        """Read default properties from a Blueprint's Class Default Object (CDO).

        Metadata discovery is the default: no property names reads one page of property
        metadata (type, category, defined_in, accepted_formats) with no values. Select
        `properties` to read their effective values, or opt in with include_values.
        For reading properties on placed actor instances in a level, use get_actor_property instead.

        Args:
            asset_path: Full asset path to the Blueprint (e.g., '/Game/Blueprints/BP_Character')
            properties: Optional list of property paths to select. A nonempty list defaults
                to an effective-value read; omit or pass an empty list for metadata discovery.
            blueprint_path: Deprecated alias for asset_path.
            include_values: Include effective values. Defaults to true for a selected read
                and false for discovery. Explicit false keeps a selected read metadata-only.
            include_members: Expand one level of struct member path/type metadata. Member
                values are only extracted when include_values is also true. Default false.
            discovery_offset: Metadata discovery page offset (default 0). Ignored by selected reads.
            discovery_limit: Metadata discovery page size (default 100, max 200). Ignored by selected reads.

        Returns:
            JSON with:
            - blueprint_path: Asset path
            - class: Generated class name
            - parent_class: Parent class name
            - values_included: Whether the response carries effective property values
            - properties: Object mapping property names to type, category, defined_in and
              accepted_formats. `value` is present only when values are included; FText
              properties also carry an optional `string_table` sibling field with {table_id, key}.
              Struct properties carry `members` only when include_members=true.
            - count: Number of properties returned
            - total_properties: Admitted population size for a discovery read
            - discovery_pagination: {offset, limit, returned, next_offset, is_complete} for a
              metadata discovery page

            Note: response currently returns key "blueprint_path" (not "asset_path").
        """
        if not asset_path:
            asset_path = blueprint_path
        if not asset_path:
            return "Error: Missing required parameter: asset_path (or blueprint_path alias)"

        try:
            params = {
                "asset_path": asset_path,
                "blueprint_path": asset_path,
            }
            if properties:
                params["properties"] = properties
            if include_values is not None:
                params["include_values"] = include_values
            if include_members is not None:
                params["include_members"] = include_members
            if discovery_offset is not None:
                params["discovery_offset"] = discovery_offset
            if discovery_limit is not None:
                params["discovery_limit"] = discovery_limit
            response = connection.send_command("blueprint.get_class_defaults", params)
            return format_response(response.get("data", {}), "get_class_defaults")
        except ConnectionError as exc:
            return f"Error: {exc}"

    @mcp.tool()
    def set_class_defaults(
        asset_path: str = "",
        properties: dict | None = None,
        compile: bool = True,
        save: bool = True,
        expected_fingerprint: dict | None = None,
        items: list[dict] | None = None,
        blueprint_path: str = "",
    ) -> str:
        """Set default property values on a Blueprint's Class Default Object (CDO).

        Configures inherited C++ UPROPERTY defaults and Blueprint variable defaults.
        Component object-reference properties accept bare names (e.g., 'OpenSeq')
        as well as historical full object paths (e.g., '/Game/.../BP.BP_C:Name_GEN_VARIABLE').
        Bare-name ambiguity is refused with AmbiguousComponentReference and a candidate list.
        Supports batch setting of multiple properties in one call.
        For setting properties on placed actor instances in a level, use set_actor_property instead.

        Args:
            asset_path: Full asset path to the Blueprint (e.g., '/Game/Blueprints/BP_Character')
            blueprint_path: Deprecated alias for asset_path.
            properties: Dictionary mapping property names to their new values.
            compile: Auto-compile the Blueprint after setting properties (default: true)
            save: Auto-save the Blueprint to disk after setting properties (default: true)
            expected_fingerprint: Optional stale-write guard for single-target mode.
            items: Optional batch item list. Each item must include target and properties,
                and may include compile, save, and expected_fingerprint.

        Returns:
            JSON with:
            - blueprint_path: Asset path
            - results: Per-property results with type, previous_value, new_value, success
            - compiled: Whether compilation was performed and succeeded
            - saved: Whether save was performed and succeeded
            - compile_errors: Array of compilation errors (if any)
            - warnings: Array of warning messages

            Note: response currently returns key "blueprint_path" (not "asset_path").
        """
        try:
            if items is not None:
                if len(items) == 0:
                    return "Error: Missing or empty required parameter: items"
                params = {"items": items}
            else:
                if not asset_path:
                    asset_path = blueprint_path
                if not asset_path:
                    return "Error: Missing required parameter: asset_path (or blueprint_path alias)"
                if not properties:
                    return "Error: Missing or empty required parameter: properties"

                params = {
                    "asset_path": asset_path,
                    "blueprint_path": asset_path,
                    "properties": properties,
                    "compile": compile,
                    "save": save,
                }
                if expected_fingerprint is not None:
                    params["expected_fingerprint"] = expected_fingerprint
            response = connection.send_command("blueprint.set_class_defaults", params)
            return format_response(response.get("data", {}), "set_class_defaults")
        except ConnectionError as exc:
            return f"Error: {exc}"
