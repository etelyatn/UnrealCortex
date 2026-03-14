"""MCP tool for Blueprint migration analysis."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

# Maps C++ override function name → Blueprint event node display_name as returned by
# graph.list_nodes. Display names are derived from the UE UFUNCTION(meta=(DisplayName=...))
# metadata on the corresponding Receive* function in Actor.h.
#
# Note: OnConstruction is intentionally absent — its entry node is UK2Node_FunctionEntry
# in the "ConstructionScript" graph, not UK2Node_Event in "EventGraph". Migrating
# OnConstruction requires a separate graph-level cleanup step.
_OVERRIDE_TO_DISPLAY_NAME: dict[str, str] = {
    "NotifyActorBeginOverlap": "Event ActorBeginOverlap",
    "NotifyActorEndOverlap": "Event ActorEndOverlap",
    "NotifyHit": "Event Hit",
    "BeginPlay": "Event BeginPlay",
    "EndPlay": "Event End Play",   # DisplayName metadata = "End Play" (with space)
    "Tick": "Event Tick",
    # Pawn/Character subclasses:
    "PossessedBy": "Event Possessed",
    "UnPossessed": "Event Unpossessed",
}


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
            response = connection.send_command("blueprint.analyze_for_migration", {
                "asset_path": asset_path,
            })
            return format_response(response.get("data", {}), "analyze_blueprint_for_migration")
        except ConnectionError as e:
            return json.dumps({"error": f"Connection error: {e}"})
        except (RuntimeError, TimeoutError, OSError) as e:
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def cleanup_blueprint_migration(
        asset_path: str,
        new_parent_class: str | None = None,
        remove_variables: list[str] | None = None,
        remove_functions: list[str] | None = None,
        migrated_overrides: list[str] | None = None,
        compile: bool = True,
    ) -> str:
        """Clean up a Blueprint after C++ migration.

        Reparent to new C++ class, remove migrated variables and functions, and
        prune dead event entry nodes whose C++ overrides have been migrated.

        Args:
            asset_path: Blueprint asset path (e.g., /Game/Blueprints/BP_Enemy)
            new_parent_class: Full class path to reparent to (e.g., /Script/MyGame.AEnemyBase)
            remove_variables: List of variable names to remove from the Blueprint
            remove_functions: List of function graph names to remove from the Blueprint
            migrated_overrides: List of C++ virtual function names (unqualified, as declared
                in the class header) whose Blueprint EventGraph entry nodes should be removed.
                Example: ["NotifyActorBeginOverlap", "BeginPlay", "Tick"]
                Known names: NotifyActorBeginOverlap, NotifyActorEndOverlap, NotifyHit,
                BeginPlay, EndPlay, Tick, PossessedBy, UnPossessed.
                Unrecognized names are skipped and reported in `unrecognized_overrides`.
                Note: OnConstruction (UserConstructionScript) is not supported here — it
                lives in the ConstructionScript graph as a UK2Node_FunctionEntry, not in
                EventGraph as UK2Node_Event.
            compile: Whether to compile the Blueprint after cleanup (default: True)

        Returns:
            JSON with fields from bp.cleanup_migration plus (when migrated_overrides provided):
            - pruned_event_nodes: count of EventGraph entry nodes removed
            - pruned_event_node_names: list of display names of removed nodes
            - unrecognized_overrides: list of override names not in the known mapping table

        Example:
            cleanup_blueprint_migration(
                asset_path="/Game/Blueprints/BP_Enemy",
                new_parent_class="/Script/MyGame.AEnemyBase",
                remove_variables=["Health"],
                migrated_overrides=["BeginPlay", "NotifyActorBeginOverlap"],
            )
            # Reparents BP, removes Health variable, removes Event BeginPlay and
            # Event ActorBeginOverlap entry nodes, deletes orphaned downstream chains,
            # then compiles.
        """
        # Defer compile when we have overrides to process after bp.cleanup_migration.
        effective_compile = False if migrated_overrides else compile
        params: dict = {"asset_path": asset_path, "compile": effective_compile}
        if new_parent_class:
            params["new_parent_class"] = new_parent_class
        if remove_variables:
            params["remove_variables"] = remove_variables
        if remove_functions:
            params["remove_functions"] = remove_functions
        try:
            response = connection.send_command("blueprint.cleanup_migration", params)
            cleanup_data = response.get("data", {})

            if not migrated_overrides:
                return format_response(cleanup_data, "cleanup_blueprint_migration")

            # Separate known mappings from unrecognized names.
            unrecognized = [n for n in migrated_overrides if n not in _OVERRIDE_TO_DISPLAY_NAME]
            target_display_names = {
                _OVERRIDE_TO_DISPLAY_NAME[name]
                for name in migrated_overrides
                if name in _OVERRIDE_TO_DISPLAY_NAME
            }

            pruned_names: list[str] = []
            if target_display_names:
                list_resp = connection.send_command("graph.list_nodes", {
                    "asset_path": asset_path,
                    "graph_name": "EventGraph",
                })
                nodes = list_resp.get("data", {}).get("nodes", [])
                for node in nodes:
                    node_id = node.get("node_id")
                    if (
                        node_id
                        and node.get("class") == "UK2Node_Event"
                        and node.get("display_name") in target_display_names
                    ):
                        connection.send_command("graph.remove_node", {
                            "asset_path": asset_path,
                            "node_id": node_id,
                            "graph_name": "EventGraph",
                        })
                        pruned_names.append(node["display_name"])

            if pruned_names:
                # Clean up orphaned downstream chains and compile.
                connection.send_command("blueprint.delete_orphaned_nodes", {
                    "asset_path": asset_path,
                    "graph_name": "EventGraph",
                    "compile": compile,
                })
            elif compile:
                # No event nodes removed but compile was deferred from bp.cleanup_migration.
                connection.send_command("blueprint.compile", {"asset_path": asset_path})

            result = dict(cleanup_data)
            result["pruned_event_nodes"] = len(pruned_names)
            result["pruned_event_node_names"] = pruned_names
            if unrecognized:
                result["unrecognized_overrides"] = unrecognized
            return format_response(result, "cleanup_blueprint_migration")

        except ConnectionError as e:
            return json.dumps({"error": f"Connection error: {e}"})
        except (RuntimeError, TimeoutError, OSError) as e:
            return json.dumps({"error": str(e)})
