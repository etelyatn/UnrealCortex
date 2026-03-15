"""MCP tools for dynamic material instance operations (runtime, PIE required)."""

import json
import logging
from typing import Any

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def register_material_dynamic_tools(mcp, connection: UEConnection):
    """Register all dynamic material instance MCP tools."""

    @mcp.tool()
    def list_dynamic_instances(actor_path: str) -> str:
        """List all material slots on a PIE actor and whether each has a Dynamic Material Instance.

        Use this to discover which components and slots are available before creating
        or modifying dynamic materials at runtime. Unlike list_instances (which lists
        saved material instance assets), this queries live actor components in PIE.

        Requires PIE Playing or Paused. Use get_pie_state to check before calling.

        Args:
            actor_path: Actor label, name, or full path in PIE world

        Returns:
            JSON with components array, each containing slots with has_dynamic_instance flag
        """
        try:
            result = connection.send_command("material.list_dynamic_instances", {"actor_path": actor_path})
            return format_response(result.get("data", {}), "list_dynamic_instances")
        except Exception as e:
            logger.error(f"list_dynamic_instances failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def get_dynamic_instance(
        actor_path: str,
        component_name: str = "",
        slot_index: int = 0,
    ) -> str:
        """Get full details of a Dynamic Material Instance including all parameters.

        Returns the parent material, all overrideable parameters (scalar, vector, texture)
        with their default values, current values, and override status. Unlike get_instance
        (which reads saved material instance assets), this reads live DMI state in PIE.

        Requires PIE Playing or Paused.
        """
        try:
            params = {"actor_path": actor_path, "slot_index": slot_index}
            if component_name:
                params["component_name"] = component_name
            result = connection.send_command("material.get_dynamic_instance", params)
            return format_response(result.get("data", {}), "get_dynamic_instance")
        except Exception as e:
            logger.error(f"get_dynamic_instance failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def create_dynamic_instance(
        actor_path: str,
        component_name: str = "",
        slot_index: int = 0,
        source_material: str = "",
        parameters: list[dict[str, Any]] | None = None,
    ) -> str:
        """Create a Dynamic Material Instance on a PIE actor's material slot.

        Unlike create_instance (which creates saved UMaterialInstanceConstant assets),
        this creates a transient runtime instance on a live actor.

        Requires PIE Playing or Paused.
        """
        try:
            params: dict[str, Any] = {"actor_path": actor_path, "slot_index": slot_index}
            if component_name:
                params["component_name"] = component_name
            if source_material:
                params["source_material"] = source_material
            if parameters:
                params["parameters"] = parameters
            result = connection.send_command("material.create_dynamic_instance", params)
            return format_response(result.get("data", {}), "create_dynamic_instance")
        except Exception as e:
            logger.error(f"create_dynamic_instance failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def destroy_dynamic_instance(
        actor_path: str,
        component_name: str = "",
        slot_index: int = 0,
    ) -> str:
        """Remove a Dynamic Material Instance and revert to the parent material.

        Reverts the material slot to the DMI's parent material. Unlike delete_instance
        (which deletes saved material instance assets), this only removes the transient
        runtime instance.

        Requires PIE Playing or Paused. Related: create_dynamic_instance, get_dynamic_instance.
        """
        try:
            params = {"actor_path": actor_path, "slot_index": slot_index}
            if component_name:
                params["component_name"] = component_name
            result = connection.send_command("material.destroy_dynamic_instance", params)
            return format_response(result.get("data", {}), "destroy_dynamic_instance")
        except Exception as e:
            logger.error(f"destroy_dynamic_instance failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def set_dynamic_parameter(
        actor_path: str,
        name: str,
        parameter_type: str,
        value: Any,
        component_name: str = "",
        slot_index: int = 0,
    ) -> str:
        """Set a material parameter on a Dynamic Material Instance at runtime.

        Unlike set_parameter (which modifies saved material instance assets), this
        changes the live DMI value visible in-game immediately. Changes are transient
        and lost when PIE stops.

        Requires PIE Playing or Paused. Related: get_dynamic_parameter, set_dynamic_parameters.
        """
        try:
            params: dict[str, Any] = {
                "actor_path": actor_path,
                "name": name,
                "type": parameter_type,
                "value": value,
                "slot_index": slot_index,
            }
            if component_name:
                params["component_name"] = component_name
            result = connection.send_command("material.set_dynamic_parameter", params)
            return format_response(result.get("data", {}), "set_dynamic_parameter")
        except Exception as e:
            logger.error(f"set_dynamic_parameter failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def get_dynamic_parameter(
        actor_path: str,
        name: str,
        component_name: str = "",
        slot_index: int = 0,
    ) -> str:
        """Get a single parameter value from a Dynamic Material Instance.

        Returns the parameter's current value, default value, type, and override status.
        Unlike get_parameter (which reads saved material instance assets), this reads
        the live DMI state in PIE.

        Requires PIE Playing or Paused. Related: list_dynamic_parameters, set_dynamic_parameter.
        """
        try:
            params = {"actor_path": actor_path, "name": name, "slot_index": slot_index}
            if component_name:
                params["component_name"] = component_name
            result = connection.send_command("material.get_dynamic_parameter", params)
            return format_response(result.get("data", {}), "get_dynamic_parameter")
        except Exception as e:
            logger.error(f"get_dynamic_parameter failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def list_dynamic_parameters(
        actor_path: str,
        component_name: str = "",
        slot_index: int = 0,
    ) -> str:
        """List all overrideable parameters on a Dynamic Material Instance.

        Returns scalar, vector, and texture parameters with their current/default
        values and override status. Unlike list_parameters (which reads saved material
        instance assets), this reads live DMI state in PIE.

        Requires PIE Playing or Paused. Related: get_dynamic_parameter, get_dynamic_instance.
        """
        try:
            params = {"actor_path": actor_path, "slot_index": slot_index}
            if component_name:
                params["component_name"] = component_name
            result = connection.send_command("material.list_dynamic_parameters", params)
            return format_response(result.get("data", {}), "list_dynamic_parameters")
        except Exception as e:
            logger.error(f"list_dynamic_parameters failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def set_dynamic_parameters(
        actor_path: str,
        parameters: list[dict[str, Any]],
        component_name: str = "",
        slot_index: int = 0,
    ) -> str:
        """Set multiple parameters on a Dynamic Material Instance in a single call.

        Batch alternative to set_dynamic_parameter. Each parameter in the array is
        applied independently — partial failures are reported per-item without
        rolling back successful ones.

        Requires PIE Playing or Paused. Related: set_dynamic_parameter, list_dynamic_parameters.
        """
        try:
            params: dict[str, Any] = {
                "actor_path": actor_path,
                "parameters": parameters,
                "slot_index": slot_index,
            }
            if component_name:
                params["component_name"] = component_name
            result = connection.send_command("material.set_dynamic_parameters", params)
            return format_response(result.get("data", {}), "set_dynamic_parameters")
        except Exception as e:
            logger.error(f"set_dynamic_parameters failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def reset_dynamic_parameter(
        actor_path: str,
        name: str,
        component_name: str = "",
        slot_index: int = 0,
    ) -> str:
        """Reset a parameter on a Dynamic Material Instance to its parent default.

        Sets the parameter value back to the parent material's default. Unlike
        reset_parameter (which clears overrides on saved material instances), this
        operates on a live DMI in PIE. Note: the parameter remains in the override
        array (UE limitation) but its value matches the default.

        Requires PIE Playing or Paused. Related: set_dynamic_parameter, get_dynamic_parameter.
        """
        try:
            params = {"actor_path": actor_path, "name": name, "slot_index": slot_index}
            if component_name:
                params["component_name"] = component_name
            result = connection.send_command("material.reset_dynamic_parameter", params)
            return format_response(result.get("data", {}), "reset_dynamic_parameter")
        except Exception as e:
            logger.error(f"reset_dynamic_parameter failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})
