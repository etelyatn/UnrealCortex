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
