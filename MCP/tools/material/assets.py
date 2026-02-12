"""MCP tools for material and material instance asset operations."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_material_asset_tools(mcp, connection: UEConnection):
    """Register all material asset-related MCP tools."""

    @mcp.tool()
    def list_materials(
        path: str = "/Game/",
        recursive: bool = True,
    ) -> str:
        """List all UMaterial assets in a directory.

        Args:
            path: Content directory path to search (default: "/Game/")
            recursive: Whether to search subdirectories recursively (default: True)

        Returns:
            JSON with:
            - materials: Array of material assets with name and asset_path
            - count: Total number of materials found
        """
        try:
            params = {"path": path, "recursive": recursive}
            result = connection.send_command("material.list_materials", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"list_materials failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def get_material(asset_path: str) -> str:
        """Get detailed information about a UMaterial asset.

        Args:
            asset_path: Full asset path to the material (e.g., "/Game/Materials/M_MyMaterial")

        Returns:
            JSON with material properties:
            - name: Material name
            - asset_path: Full asset path
            - material_domain: Domain (Surface, DeferredDecal, LightFunction, PostProcess, UI)
            - blend_mode: Blend mode (Opaque, Masked, Translucent, Additive, Modulate)
            - shading_model: Shading model (Unlit, DefaultLit, Subsurface, ClearCoat, etc.)
            - two_sided: Whether material is two-sided
            - is_masked: Whether material uses masking
            - node_count: Number of expression nodes in material graph
            - parameter_count: Object with counts of scalar, vector, and texture parameters
        """
        try:
            params = {"asset_path": asset_path}
            result = connection.send_command("material.get_material", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"get_material failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def create_material(
        asset_path: str,
        name: str,
    ) -> str:
        """Create a new UMaterial asset.

        Args:
            asset_path: Directory path where the material will be created
            name: Name for the new material (should start with M_)

        Returns:
            JSON with:
            - asset_path: Full path to created material
            - name: Material name
            - created: True if successful
        """
        try:
            params = {"asset_path": asset_path, "name": name}
            result = connection.send_command("material.create_material", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"create_material failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def delete_material(asset_path: str) -> str:
        """Delete a UMaterial asset.

        Args:
            asset_path: Full asset path to the material to delete

        Returns:
            JSON with:
            - asset_path: Path of deleted material
            - deleted: True if successful
        """
        try:
            params = {"asset_path": asset_path}
            result = connection.send_command("material.delete_material", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"delete_material failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def list_instances(
        path: str = "/Game/",
        parent_material: str = "",
    ) -> str:
        """List all UMaterialInstanceConstant assets.

        Args:
            path: Content directory path to search (default: "/Game/")
            parent_material: Optional filter - only return instances of this parent material

        Returns:
            JSON with:
            - instances: Array of material instance assets with name and asset_path
            - count: Total number of instances found
        """
        try:
            params = {"path": path}
            if parent_material:
                params["parent_material"] = parent_material
            result = connection.send_command("material.list_instances", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"list_instances failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def get_instance(asset_path: str) -> str:
        """Get detailed information about a material instance.

        Args:
            asset_path: Full asset path to the material instance

        Returns:
            JSON with instance properties:
            - name: Instance name
            - asset_path: Full asset path
            - parent_material: Path to parent material
            - overrides: Object containing arrays of scalar, vector, and texture parameter overrides
        """
        try:
            params = {"asset_path": asset_path}
            result = connection.send_command("material.get_instance", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"get_instance failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def create_instance(
        asset_path: str,
        name: str,
        parent_material: str,
    ) -> str:
        """Create a new UMaterialInstanceConstant asset.

        Args:
            asset_path: Directory path where the instance will be created
            name: Name for the new instance (should start with MI_)
            parent_material: Full asset path to the parent UMaterial

        Returns:
            JSON with:
            - asset_path: Full path to created instance
            - name: Instance name
            - parent_material: Parent material path
            - created: True if successful
        """
        try:
            params = {
                "asset_path": asset_path,
                "name": name,
                "parent_material": parent_material,
            }
            result = connection.send_command("material.create_instance", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"create_instance failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def delete_instance(asset_path: str) -> str:
        """Delete a material instance asset.

        Args:
            asset_path: Full asset path to the material instance to delete

        Returns:
            JSON with:
            - asset_path: Path of deleted instance
            - deleted: True if successful
        """
        try:
            params = {"asset_path": asset_path}
            result = connection.send_command("material.delete_instance", params)
            return format_response(result)
        except Exception as e:
            logger.error(f"delete_instance failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})
