"""MCP tools for Blueprint migration operations."""

import json
import logging

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def register_blueprint_migration_tools(mcp, connection: UEConnection):
    """Register Blueprint migration MCP tools."""

    @mcp.tool()
    def rename_blueprint(source_path: str, dest_path: str) -> str:
        """Rename or move a Blueprint asset.

        Args:
            source_path: Existing Blueprint object path.
            dest_path: Destination Blueprint object path.
        """
        try:
            response = connection.send_command("bp.rename", {
                "source_path": source_path,
                "dest_path": dest_path,
            })
            return format_response(response.get("data", {}), "rename_blueprint")
        except ConnectionError as e:
            return json.dumps({"error": f"Connection error: {e}"})
        except (RuntimeError, TimeoutError, OSError) as e:
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def fixup_redirectors(path: str, recursive: bool = True) -> str:
        """Fix redirectors in a content path.

        Args:
            path: Content path to scan.
            recursive: Whether to include subdirectories.
        """
        try:
            response = connection.send_command("bp.fixup_redirectors", {
                "path": path,
                "recursive": recursive,
            })
            return format_response(response.get("data", {}), "fixup_redirectors")
        except ConnectionError as e:
            return json.dumps({"error": f"Connection error: {e}"})
        except (RuntimeError, TimeoutError, OSError) as e:
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def recompile_dependent_blueprints(asset_path: str) -> str:
        """Recompile Blueprints that depend on the given Blueprint."""
        try:
            response = connection.send_command("bp.recompile_dependents", {
                "asset_path": asset_path,
            })
            return format_response(response.get("data", {}), "recompile_dependent_blueprints")
        except ConnectionError as e:
            return json.dumps({"error": f"Connection error: {e}"})
        except (RuntimeError, TimeoutError, OSError) as e:
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def compare_blueprints(
        source_path: str,
        target_path: str,
        sections: list[str] | None = None,
    ) -> str:
        """Compare two Blueprints structurally and return differences."""
        params: dict[str, object] = {
            "source_path": source_path,
            "target_path": target_path,
        }
        if sections:
            params["sections"] = sections

        try:
            response = connection.send_command("bp.compare_blueprints", params)
            return format_response(response.get("data", {}), "compare_blueprints")
        except ConnectionError as e:
            return json.dumps({"error": f"Connection error: {e}"})
        except (RuntimeError, TimeoutError, OSError) as e:
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def remove_scs_component(
        asset_path: str,
        component_name: str,
        compile: bool = True,
    ) -> str:
        """Remove an SCS component node from a Blueprint.

        Use after migrating a component to a C++ UPROPERTY. Children of the
        removed node are re-parented to its parent automatically.

        Args:
            asset_path: Blueprint object path (e.g. /Game/Blueprints/BP_Foo).
            component_name: SCS variable name of the component to remove.
            compile: Recompile the Blueprint after removal (default True).
        """
        try:
            response = connection.send_command("bp.remove_scs_component", {
                "asset_path": asset_path,
                "component_name": component_name,
                "compile": compile,
            })
            return format_response(response.get("data", {}), "remove_scs_component")
        except ConnectionError as e:
            return json.dumps({"error": f"Connection error: {e}"})
        except (RuntimeError, TimeoutError, OSError) as e:
            return json.dumps({"error": str(e)})
