"""MCP tool for Blueprint migration analysis."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_blueprint_analysis_tools(mcp, connection: UEConnection):
    """Register Blueprint analysis MCP tools."""

    @mcp.tool()
    def analyze_blueprint_for_migration(asset_path: str) -> str:
        """Analyze a Blueprint for C++ migration in a single call.

        Returns migration-ready analysis including Blueprint metadata, variables with usage
        counts, functions with purity/latent flags, components, graph/event breakdown,
        timelines, event dispatchers, implemented interfaces, latent node summary,
        and complexity metrics.

        Args:
            asset_path: Full Blueprint asset path (e.g. "/Game/Blueprints/BP_Player")

        Returns:
            JSON object from `bp.analyze_for_migration`.
        """
        try:
            response = connection.send_command("bp.analyze_for_migration", {
                "asset_path": asset_path,
            })
            return format_response(response.get("data", {}), "analyze_blueprint_for_migration")
        except ConnectionError as e:
            return json.dumps({"error": f"Connection error: {e}"})
        except (RuntimeError, TimeoutError, OSError) as e:
            return json.dumps({"error": str(e)})

    @mcp.tool()
    async def cleanup_blueprint_migration(
        asset_path: str,
        new_parent_class: str | None = None,
        remove_variables: list[str] | None = None,
        remove_functions: list[str] | None = None,
        compile: bool = True,
    ) -> str:
        """Clean up a Blueprint after C++ migration.

        Reparent to new C++ class, remove migrated variables and functions.
        All operations are transaction-wrapped for undo support.

        Args:
            asset_path: Blueprint asset path (e.g., /Game/Blueprints/BP_Enemy)
            new_parent_class: Full class path to reparent to (e.g., /Script/MyGame.AEnemyBase)
            remove_variables: List of variable names to remove from the Blueprint
            remove_functions: List of function graph names to remove from the Blueprint
            compile: Whether to compile the Blueprint after cleanup (default: True)
        """
        params = {"asset_path": asset_path, "compile": compile}
        if new_parent_class:
            params["new_parent_class"] = new_parent_class
        if remove_variables:
            params["remove_variables"] = remove_variables
        if remove_functions:
            params["remove_functions"] = remove_functions
        return await connection.send_command("bp.cleanup_migration", params)
