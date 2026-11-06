"""MCP tools for Blueprint Class Settings operations (interfaces, tick, replication)."""

import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_blueprint_class_settings_tools(mcp, connection: UEConnection):
    """Register all Blueprint class settings MCP tools."""

    @mcp.tool()
    def add_blueprint_interface(
        asset_path: str,
        interface_path: str,
        compile: bool = True,
    ) -> str:
        """Add an interface implementation to a Blueprint.

        Adds the interface to the Blueprint's Implemented Interfaces list
        and generates stub function graphs for interface methods.

        Args:
            asset_path: Full path to the Blueprint (e.g., '/Game/Blueprints/BP_MyActor')
            interface_path: Interface class name or path. Accepts:
                - Short C++ name: 'ActorTickableInterface'
                - Blueprint interface path: '/Game/Interfaces/BPI_MyInterface'
            compile: Recompile after adding (default: true)

        Returns:
            JSON with:
            - asset_path: Blueprint path
            - interface_name: Resolved interface class name
            - interface_path: Full interface class path
            - compiled: Whether compilation succeeded
            - stub_functions: Array of generated stub function graph names
        """
        try:
            params = {
                "asset_path": asset_path,
                "interface_path": interface_path,
                "compile": compile,
            }
            response = connection.send_command("blueprint.add_interface", params)
            return format_response(response.get("data", {}), "add_blueprint_interface")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def remove_blueprint_interface(
        asset_path: str,
        interface_path: str,
        preserve_implementations: bool,
        dry_run: bool = False,
        compile: bool = True,
        expected_fingerprint: dict | None = None,
        expected_validation_hash: str | None = None,
    ) -> str:
        """Preview or remove an interface implementation from a Blueprint.

        preserve_implementations is required and states whether loss is accepted:
        true keeps interface function graphs as ordinary function graphs and turns
        interface events into same-named custom events with their pin links; false
        deletes the graphs and leaves event bodies orphaned (explicit loss).

        Always run dry_run=true first. Apply (dry_run=false) requires
        expected_fingerprint (the dry run's fingerprint_before) and
        expected_validation_hash (the dry run's validation_hash); a stale value is
        refused with STALE_PRECONDITION. Apply verifies the predicted disposition on
        the live graphs before compiling and restores the verified pre-state on a
        mismatch (READBACK_MISMATCH) or a compile error (COMPILE_FAILED).

        Args:
            asset_path: Full path to the Blueprint
            interface_path: Interface class name or path (same formats as add_blueprint_interface)
            preserve_implementations: true=keep implementations as functions/custom events, false=delete them
            dry_run: true=preview only (default false)
            compile: Recompile after removing (default: true)
            expected_fingerprint: Apply-required; fingerprint_before from the dry run
            expected_validation_hash: Apply-required; validation_hash from the dry run

        Returns:
            JSON with:
            - asset_path, interface_name, interface_path, preserve_implementations
            - functions: per interface function {function, implementation
              (function_graph|event|none), node_count, disposition
              (promoted_to_function|converted_to_custom_event|deleted|orphaned_nodes),
              in_asset_callers}
            - validation_hash and fingerprint_before (dry run), compiled and
              removed_graphs/preserved_graphs (apply)
        """
        try:
            params: dict[str, object] = {
                "asset_path": asset_path,
                "interface_path": interface_path,
                "preserve_implementations": preserve_implementations,
                "dry_run": dry_run,
                "compile": compile,
            }
            if expected_fingerprint is not None:
                params["expected_fingerprint"] = expected_fingerprint
            if expected_validation_hash is not None:
                params["expected_validation_hash"] = expected_validation_hash
            response = connection.send_command("blueprint.remove_interface", params)
            return format_response(response.get("data", {}), "remove_blueprint_interface")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_blueprint_tick_settings(
        asset_path: str,
        start_with_tick_enabled: bool | None = None,
        can_ever_tick: bool | None = None,
        tick_interval: float | None = None,
        compile: bool = True,
        save: bool = False,
    ) -> str:
        """Set Actor tick settings on a Blueprint's Class Defaults.

        Only applies to Actor-based Blueprints. All parameters except
        asset_path are optional — only provided values are changed.

        Prefer this over set_class_defaults for tick settings because it
        auto-sets bCanEverTick when enabling tick and validates Actor type.

        Args:
            asset_path: Full path to the Blueprint
            start_with_tick_enabled: Enable tick at start. When true, also
                forces bCanEverTick=true. When false, only clears
                bStartWithTickEnabled (bCanEverTick stays for runtime re-enable).
            can_ever_tick: Independent control over whether ticking is possible
                at all. Usually set automatically via start_with_tick_enabled.
            tick_interval: Tick interval in seconds (0 = every frame)
            compile: Recompile after setting (default: true)
            save: Save after setting (default: false)

        Returns:
            JSON with:
            - asset_path: Blueprint path
            - start_with_tick_enabled: Current value
            - can_ever_tick: Current value
            - tick_interval: Current value
            - compiled: Whether compilation succeeded
            - saved: Whether save succeeded
        """
        try:
            params = {"asset_path": asset_path, "compile": compile, "save": save}
            if start_with_tick_enabled is not None:
                params["start_with_tick_enabled"] = start_with_tick_enabled
            if can_ever_tick is not None:
                params["can_ever_tick"] = can_ever_tick
            if tick_interval is not None:
                params["tick_interval"] = tick_interval
            response = connection.send_command("blueprint.set_tick_settings", params)
            return format_response(response.get("data", {}), "set_blueprint_tick_settings")
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def set_blueprint_replication_settings(
        asset_path: str,
        replicates: bool | None = None,
        replicate_movement: bool | None = None,
        net_dormancy: str | None = None,
        net_use_owner_relevancy: bool | None = None,
        compile: bool = True,
        save: bool = False,
    ) -> str:
        """Set replication settings on a Blueprint's Class Defaults.

        Only applies to Actor-based Blueprints. All parameters except
        asset_path are optional — only provided values are changed.

        Prefer this over set_class_defaults for replication settings because
        it validates Actor type and provides enum validation for net_dormancy.

        Args:
            asset_path: Full path to the Blueprint
            replicates: Enable replication for this actor
            replicate_movement: Replicate movement (requires replicates=true)
            net_dormancy: Net dormancy mode. One of:
                DORM_Never, DORM_Awake, DORM_DormantAll,
                DORM_DormantPartial, DORM_Initial
            net_use_owner_relevancy: Use owner relevancy for net culling
            compile: Recompile after setting (default: true)
            save: Save after setting (default: false)

        Returns:
            JSON with:
            - asset_path: Blueprint path
            - replicates: Current value
            - replicate_movement: Current value
            - net_dormancy: Current dormancy string
            - net_use_owner_relevancy: Current value
            - compiled: Whether compilation succeeded
            - saved: Whether save succeeded
        """
        try:
            params = {"asset_path": asset_path, "compile": compile, "save": save}
            if replicates is not None:
                params["replicates"] = replicates
            if replicate_movement is not None:
                params["replicate_movement"] = replicate_movement
            if net_dormancy is not None:
                params["net_dormancy"] = net_dormancy
            if net_use_owner_relevancy is not None:
                params["net_use_owner_relevancy"] = net_use_owner_relevancy
            response = connection.send_command("blueprint.set_replication_settings", params)
            return format_response(
                response.get("data", {}), "set_blueprint_replication_settings"
            )
        except ConnectionError as e:
            return f"Error: {e}"
