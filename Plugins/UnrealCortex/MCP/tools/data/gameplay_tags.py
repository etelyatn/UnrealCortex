"""MCP tools for GameplayTag operations."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_TTL_LIST = 300  # 5 min


def register_gameplay_tag_tools(mcp, connection: UEConnection):
    """Register all GameplayTag-related MCP tools."""

    @mcp.tool()
    def list_gameplay_tags(prefix: str = "", include_source_file: bool = False) -> str:
        """List all registered GameplayTags, optionally filtered by prefix.

        Each tag entry includes parent_tags (ancestor chain) and child_tags (direct children).
        Use include_source_file=True to also get the .ini file that defines each tag.

        Use validate_gameplay_tag to check if a specific tag exists.

        Args:
            prefix: Only return tags starting with this prefix (e.g., 'Quest.', 'Patient.Type.').
            include_source_file: If true, include the source .ini file path for each tag.

        Returns:
            JSON with:
            - tags: Array of {tag, parent_tags[], child_tags[], source_file?}
            - count: Total number of matching tags
        """
        try:
            params = {"prefix": prefix}
            if include_source_file:
                params["include_source_file"] = True
            # Don't cache when requesting source files (less common, more dynamic)
            if include_source_file:
                response = connection.send_command("data.list_gameplay_tags", params)
            else:
                response = connection.send_command_cached(
                    "data.list_gameplay_tags", params, ttl=_TTL_LIST
                )
            return format_response(response.get("data", {}), "list_gameplay_tags")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def validate_gameplay_tag(tag: str) -> str:
        """Check if a GameplayTag is registered and valid.

        Use register_gameplay_tag to add missing tags.

        Args:
            tag: The tag string to validate (e.g., 'Quest.Generic.1').

        Returns:
            JSON with:
            - tag: The tag string that was checked
            - valid: Boolean indicating whether the tag is registered
        """
        try:
            response = connection.send_command("data.validate_gameplay_tag", {"tag": tag})
            return format_response(response.get("data", {}), "validate_gameplay_tag")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def register_gameplay_tag(tag: str, ini_file: str = "", dev_comment: str = "") -> str:
        """Register a new GameplayTag in the project.

        The tag is written to a .ini file in Config/Tags/. The target file is determined by:
        1. The ini_file parameter (if provided)
        2. The project's tag prefix mapping in plugin settings
        3. Default: Config/Tags/GameplayTags.ini

        Tag format: alphanumeric characters, dots, and underscores only (e.g., 'Quest.New_Category.1').

        Args:
            tag: Tag string to register (e.g., 'Quest.NewCategory.SubTag').
            ini_file: Override the target .ini file path (relative to Config/).
            dev_comment: Developer comment for the tag entry.

        Returns:
            JSON with:
            - tag: The registered tag string
            - ini_file: The .ini file the tag was written to
            - already_existed: Boolean, true if the tag was already registered
        """
        try:
            params = {"tag": tag}
            if ini_file:
                params["ini_file"] = ini_file
            if dev_comment:
                params["dev_comment"] = dev_comment
            response = connection.send_command("data.register_gameplay_tag", params)
            # Invalidate tag list and catalog caches
            connection.invalidate_cache("data.list_gameplay_tags:")
            connection.invalidate_cache("get_data_catalog:")
            return format_response(response.get("data", {}), "register_gameplay_tag")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def register_gameplay_tags(tags: str) -> str:
        """Batch register multiple GameplayTags.

        Each tag follows the same rules as register_gameplay_tag.

        Args:
            tags: JSON string with array of objects, each containing 'tag' and optionally
                  'ini_file' and 'dev_comment'.
                  Example: '[{"tag": "Quest.New.1"}, {"tag": "Quest.New.2", "dev_comment": "Second quest"}]'

        Returns:
            JSON with:
            - registered: Number of newly registered tags
            - already_existed: Number of tags that were already registered
            - failed: Number of tags that failed to register
            - results: Array with per-tag details
        """
        try:
            tags_data = json.loads(tags)
            response = connection.send_command("data.register_gameplay_tags", {"tags": tags_data})
            # Invalidate tag list and catalog caches
            connection.invalidate_cache("data.list_gameplay_tags:")
            connection.invalidate_cache("get_data_catalog:")
            return format_response(response.get("data", {}), "register_gameplay_tags")
        except json.JSONDecodeError as e:
            return f"Error: Invalid JSON in tags: {e}"
        except ConnectionError as e:
            return f"Error: {e}"
