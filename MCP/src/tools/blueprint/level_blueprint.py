"""MCP tool for accessing Level Script Blueprints."""

import json
from cortex_mcp.tcp_client import UEConnection


def register_level_blueprint_tools(mcp, connection: UEConnection):
    """Register Level Blueprint MCP tools."""

    @mcp.tool()
    def get_level_blueprint(map_path: str) -> str:
        """Get the Level Script Blueprint for a map.

        Level Script Blueprints live inside map packages (ULevel::LevelScriptBlueprint)
        — not standalone .uasset files. This tool returns a synthetic asset path that
        works with all graph_* and bp.* commands.

        Args:
            map_path: Map package path, e.g. '/Game/Maps/TestMap'.
                      This is the MAP path, not a Blueprint path.
                      Trailing slash is stripped automatically.

        Returns JSON with:
            asset_path: Synthetic path for use with graph_* and bp.* commands.
                        Format: '__level_bp__:/Game/Maps/TestMap'
            map_path: The original (normalized) map path.
            is_level_blueprint: true
            save_warning: Instruction for persisting changes.

        Example:
            result = get_level_blueprint(map_path="/Game/Maps/TestMap")
            # Use result["asset_path"] with graph_list_graphs, graph_add_node, etc.
            # To save changes: use save_level with result["map_path"]

        Note: ForEach loops use CallFunction with
              function_name: "KismetArrayLibrary.Array_ForEach"
        """
        normalized = map_path.rstrip("/")
        if not normalized.startswith("/"):
            normalized = "/" + normalized

        synthetic_path = f"__level_bp__:{normalized}"

        return json.dumps({
            "asset_path": synthetic_path,
            "map_path": normalized,
            "is_level_blueprint": True,
            "save_warning": (
                f"Level Blueprint changes are saved with save_level(map_path='{normalized}'), "
                f"not bp.save. Call save_level after editing the Level Blueprint."
            ),
        })
