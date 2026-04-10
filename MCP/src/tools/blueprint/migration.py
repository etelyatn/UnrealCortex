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
            response = connection.send_command("blueprint.rename", {
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
            response = connection.send_command("blueprint.fixup_redirectors", {
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
            response = connection.send_command("blueprint.recompile_dependents", {
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
            response = connection.send_command("blueprint.compare_blueprints", params)
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
        acknowledged_losses: list[str] | None = None,
        force: bool = False,
    ) -> str:
        """Remove an SCS component node from a Blueprint.

        Use after migrating a component to a C++ UPROPERTY. Children of the
        removed node are re-parented to its parent automatically.
        Safety is tiered: clean templates proceed, top-level property drift
        returns a diff, and instanced sub-object drift requires explicit loss
        acknowledgment or force override.

        Args:
            asset_path: Blueprint object path (e.g. /Game/Blueprints/BP_Foo).
            component_name: SCS variable name of the component to remove.
            compile: Recompile the Blueprint after removal (default True).
            acknowledged_losses: Exact keys echoed from required_acknowledgment.
            force: Override dirty-state protection and remove anyway.
        """
        try:
            params: dict[str, object] = {
                "asset_path": asset_path,
                "component_name": component_name,
                "compile": compile,
            }
            if acknowledged_losses is not None:
                params["acknowledged_losses"] = acknowledged_losses
            if force:
                params["force"] = force

            response = connection.send_command("blueprint.remove_scs_component", params)
            return format_response(response.get("data", {}), "remove_scs_component")
        except ConnectionError as e:
            return json.dumps({"error": f"Connection error: {e}"})
        except (RuntimeError, TimeoutError, OSError) as e:
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def rename_scs_component(
        asset_path: str,
        old_name: str,
        new_name: str,
        compile: bool = True,
    ) -> str:
        """Rename an SCS component node on a Blueprint.

        Escape hatch for SCS/inherited-name collisions during migration. Refuses
        inherited targets, timeline components, components that reference local
        timelines, and collisions against existing SCS nodes, Blueprint
        variables, inherited UPROPERTY members, or dependent Blueprints that
        would be silently shadowed.

        Args:
            asset_path: Blueprint object path (e.g. /Game/Blueprints/BP_Foo).
            old_name: Current SCS variable name to rename.
            new_name: New SCS variable name to apply.
            compile: Recompile the Blueprint after rename (default True).
        """
        try:
            response = connection.send_command("blueprint.rename_scs_component", {
                "asset_path": asset_path,
                "old_name": old_name,
                "new_name": new_name,
                "compile": compile,
            })
            return format_response(response.get("data", {}), "rename_scs_component")
        except ConnectionError as e:
            return json.dumps({"error": f"Connection error: {e}"})
        except (RuntimeError, TimeoutError, OSError) as e:
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def delete_orphaned_nodes(
        asset_path: str,
        graph_name: str,
        compile: bool = True,
    ) -> str:
        """Delete orphaned nodes from a Blueprint event graph.

        Orphaned nodes are those not reachable from any event entry node via
        exec pin chains. Event entry nodes are always preserved.

        Args:
            asset_path: Blueprint object path (e.g. /Game/Blueprints/BP_Foo).
            graph_name: Name of the graph to clean (e.g. "EventGraph").
            compile: Recompile the Blueprint after cleanup (default True).
        """
        try:
            response = connection.send_command("blueprint.delete_orphaned_nodes", {
                "asset_path": asset_path,
                "graph_name": graph_name,
                "compile": compile,
            })
            return format_response(response.get("data", {}), "delete_orphaned_nodes")
        except ConnectionError as e:
            return json.dumps({"error": f"Connection error: {e}"})
        except (RuntimeError, TimeoutError, OSError) as e:
            return json.dumps({"error": str(e)})
