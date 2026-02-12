"""Material tools for MCP."""

from .assets import register_material_asset_tools
from .parameters import register_material_parameter_tools

__all__ = [
    "register_material_asset_tools",
    "register_material_parameter_tools",
]
