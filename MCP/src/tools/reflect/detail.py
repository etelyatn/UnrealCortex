"""MCP tools for class detail queries."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_DETAIL = 1800  # 30 min

# Fields included at each detail level
# summary: enough to understand relationships
_SUMMARY_FIELDS = {
    "name", "type", "parent", "module", "asset_path",
    "source_path", "blueprint_children_count",
}
# properties: + structural data for working with variables
_PROPERTIES_FIELDS = _SUMMARY_FIELDS | {"properties", "interfaces", "components"}
# full: + function signatures (source snippets deferred from MVP)
_FULL_FIELDS = _PROPERTIES_FIELDS | {"functions"}

# Coverage/error metadata is not a presentation level: a native reason why a read is incomplete
# (an unreadable CDO, a partial result, an error envelope) must survive every projection, or the
# caller cannot tell a genuinely empty result from an unreadable one.
_COVERAGE_FIELDS = {"cdo_unavailable", "_message", "error", "error_code"}

# Standalone detail levels, and the native extraction mode each one forwards.
_VALID_DETAILS = ("summary", "properties", "full")
# Native implements summary and full only; the properties view is the native full extraction
# followed by the existing Python allowlist, so only 'summary' requests the reduced native read.
_NATIVE_DETAIL = {"summary": "summary", "properties": "full", "full": "full"}


def _prune_detail(data: dict, detail: str) -> dict:
    """Prune response fields based on detail level, always keeping coverage/error metadata."""
    if detail == "full":
        allowed = _FULL_FIELDS
    elif detail == "properties":
        allowed = _PROPERTIES_FIELDS
    else:
        allowed = _SUMMARY_FIELDS

    allowed = allowed | _COVERAGE_FIELDS
    return {k: v for k, v in data.items() if k in allowed}


def register_reflect_detail_tools(mcp, connection: UEConnection):
    """Register class detail query tools."""

    @mcp.tool()
    def query_class_detail(
        class_name: str,
        include_inherited: bool = False,
        detail: str = "summary",
    ) -> str:
        """Use when you need complete information about a single class.

        Returns properties, functions, interfaces, components and source file path. `detail` is
        'summary' (default; name/type/parent/routing only, forwarded to the native summary read
        before any member extraction), 'properties' (property list) or 'full' (everything including
        function signatures); `include_inherited=true` adds inherited members. Any other `detail`
        value is refused without contacting the editor.
        """
        if detail not in _VALID_DETAILS:
            return (
                f"Error: invalid detail '{detail}'; expected one of "
                f"{', '.join(_VALID_DETAILS)}"
            )
        try:
            response = connection.send_command_cached(
                "reflect.class_detail",
                {
                    "class_name": class_name,
                    "include_inherited": include_inherited,
                    "detail": _NATIVE_DETAIL[detail],
                },
                ttl=_TTL_DETAIL,
            )
            data = response.get("data", {})
            pruned = _prune_detail(data, detail)
            return format_response(pruned, "query_class_detail")
        except ConnectionError as e:
            return f"Error: {e}"
